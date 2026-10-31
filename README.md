# FastCopy-Win

Motor de copia y movimiento de archivos propio para el **Explorador de Windows 11**, que reemplaza al de Windows sin cambiar cómo lo usas: arrastrar, `Ctrl+C`, `Ctrl+X`, `Ctrl+V` y el menú contextual siguen funcionando igual, pero la copia la hace FastCopy.

> **Estado: experimental.** Funciona y está probado en una sola máquina (Windows 11 IoT Enterprise LTSC 2024, build 26100.6713). Úsalo bajo tu propia responsabilidad y lee la sección [Riesgos](#riesgos).

## Por qué

El copiado del Explorador es lento con muchos archivos, sobre todo entre discos rápidos (NVMe), porque copia casi en serie y pasa todo por la caché del sistema. FastCopy copia en paralelo y sin caché para archivos grandes.

En una prueba copiando una carpeta de **3001 archivos (2,1 GB)** de un NVMe a otro, con el mismo gesto del Explorador (`Ctrl+C` / `Ctrl+V`):

| | Tiempo |
|---|---|
| Windows (nativo) | 7,20 s |
| FastCopy | 1,49 s (~4,8× más rápido) |

Ambas copias con el origen en caché y verificadas: mismos archivos y mismos bytes. Es una sola prueba en un solo equipo; con archivos enormes en frío la diferencia será menor, porque ahí manda el disco.

## Qué hace

- **Se siente nativo:** no hay que abrir ningún programa. Arrastrar (botón izquierdo o derecho), cortar, copiar y pegar en el Explorador pasan por FastCopy.
- **Copia en paralelo** (hasta 8 archivos a la vez) y **sin caché** para archivos de más de 256 MB.
- **Mover seguro entre discos:** copia, **verifica byte a byte** y solo entonces borra el original. Si algo falla, el original se queda donde estaba.
- **Mover dentro del mismo disco** es un renombrado instantáneo, como en Windows.
- **Ventana de progreso** estilo Windows 11 (tema claro/oscuro, color de acento) con velocidad, tiempo restante, botones de icono para **pausar/reanudar** y **cancelar** cada operación, y progreso en el icono de la barra de tareas. Solo aparece si la operación dura más de medio segundo.
- **Varias operaciones en una sola ventana**, como en Windows: un panel por copia o movimiento, que entra y sale con animación.
- **Más detalles:** la barra de progreso se transforma en una gráfica de velocidad de toda la operación (con media y máxima), y aparecen elementos procesados y restantes, datos, tiempo transcurrido y restante, y la ruta. Recuerda si lo dejaste abierto. Las animaciones respetan la opción "Efectos de animación" de Windows.
- **Si no lo tiene claro, se lo deja a Windows.** Antes de escribir nada, FastCopy planifica toda la operación. Si encuentra algo que no maneja, no toca nada y la copia la hace Windows como siempre:
  - conflictos de nombre (sale el diálogo normal de "Reemplazar u omitir"),
  - rutas de red, móviles, ZIP y otras carpetas virtuales,
  - enlaces simbólicos y puntos de unión,
  - rutas de más de 260 caracteres,
  - falta de espacio en el destino,
  - operaciones que mezclan copiar con borrar, renombrar, etc.

## Cómo funciona

```
 Explorador (explorer.exe)
 ┌──────────────────────────────────────────────┐
 │  arrastrar / Ctrl+V / menú contextual         │
 │        │                                      │
 │        ▼                                      │
 │  IFileOperation  ──hook──►  fc_hook.dll       │
 │  (CopyItems, MoveItems,     ├─ anota qué copiar
 │   PerformOperations)        ├─ motor propio (paralelo, verificación)
 │                             └─ ventana de progreso
 └──────────────────────────────────────────────┘
          ▲
          │ carga la DLL al iniciar sesión y si el Explorador se reinicia
   fc_agent.exe  (tu usuario, sin privilegios)
```

1. El Explorador usa la interfaz pública de Windows `IFileOperation` (en `windows.storage.dll`) para copiar y mover. FastCopy intercepta sus métodos con [MinHook](https://github.com/TsudaKageyu/minhook).
2. Mientras el Explorador va encolando archivos (`CopyItems`, `MoveItems`), FastCopy los anota. Windows también los encola, para poder devolverle la operación si hace falta.
3. Cuando el Explorador pide ejecutar (`PerformOperations`), FastCopy hace la copia con su motor y le responde "hecho". Si la operación no es de las que maneja, llama al `PerformOperations` original y copia Windows.
4. `fc_agent.exe` corre con tu usuario (una tarea programada al iniciar sesión) y se asegura de que la DLL esté cargada en el Explorador, también después de reiniciarlo.

## Requisitos

- Windows 11 x64. Probado en 24H2 (build 26100). En otras versiones debería funcionar, porque usa una interfaz pública, pero no está probado.
- Para compilar: Visual Studio 2022 Build Tools con el componente de C++ (MSVC) y el Windows SDK.
- Permisos de administrador solo para instalar (copiar a `Program Files`).

## Compilar

```powershell
git clone --recursive https://github.com/Daniboy2116/FastCopy-Win.git
cd FastCopy-Win
.\build.bat
```

`--recursive` es necesario: MinHook está como submódulo en `third_party/minhook`. Si ya clonaste sin él:

```powershell
git submodule update --init
```

`build.bat` genera en `bin\`:

| Archivo | Qué es |
|---|---|
| `fc_hook.dll` | El hook, el motor y la ventana de progreso; se carga dentro del Explorador |
| `fc_agent.exe` | El agente que carga la DLL en el Explorador |
| `engine_test.exe` | Tests del motor (no toca el Explorador) |

Para pasar los tests:

```powershell
.\bin\engine_test.exe
```

Usan una carpeta temporal y, si existe, el disco `D:` para probar entre volúmenes.

## Instalar y desinstalar

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

- Copia los archivos a `C:\Program Files\FastCopy` (pedirá permiso de administrador una vez).
- Registra la tarea programada `FastCopyAgent`, que arranca al iniciar sesión con tu usuario.
- Arranca el agente. Si había una versión anterior cargada, reinicia el Explorador (se cierran las ventanas de carpetas abiertas).

Para quitarlo todo y volver al copiado de Windows:

```powershell
powershell -ExecutionPolicy Bypass -File .\uninstall.ps1
```

### Apagado de emergencia

Si algo va mal, crea un archivo vacío llamado `fastcopy_off` en tu carpeta temporal y todas las operaciones vuelven a ser de Windows al instante:

```powershell
New-Item "$env:TEMP\fastcopy_off" -ItemType File
```

Bórralo para volver a activar FastCopy. Reiniciar el Explorador sin el agente también lo desactiva.

### Registro

Todo lo que hace FastCopy queda en `%TEMP%\fastcopy.log`: qué operaciones hizo él, cuáles devolvió a Windows y por qué, y la velocidad de cada una.

## Riesgos

Lee esto antes de instalarlo:

- **Se ejecuta código dentro del Explorador.** Un fallo en la DLL puede cerrar el Explorador. Windows lo reinicia solo y el agente vuelve a cargar FastCopy, pero puede pasar.
- **Antivirus y Smart App Control.** Cargar una DLL en otro proceso es una técnica que también usa el malware. Defender no lo bloqueó en las pruebas, pero otros antivirus o Smart App Control en modo "Activado" podrían hacerlo, porque los binarios no están firmados.
- **El `Ctrl+Z` del Explorador no deshace** las copias y movimientos que hizo FastCopy.
- **Solo comprobado en una máquina.** Por ahora, los datos importantes: con copia de seguridad.

FastCopy está diseñado para no perder datos: nunca borra un original de un movimiento sin haber verificado la copia byte a byte, y ante cualquier duda deja la operación en manos de Windows.

## Estructura del proyecto

```
src/
  hook/fchook.c    hooks de IFileOperation dentro del Explorador
  hook/ui.c        ventana de progreso
  engine/engine.c  motor de copia/movimiento (planificación, copia paralela, verificación)
  engine/log.c     registro en %TEMP%\fastcopy.log
  agent/agent.c    agente que carga la DLL en el Explorador
tests/
  engine_test.c    tests del motor
third_party/
  minhook/         submódulo de MinHook
build.bat          compila todo en bin\
install.ps1        instala en Program Files y registra la tarea de inicio
uninstall.ps1      lo quita todo
```

## Pendiente

- Rutas de más de 260 caracteres (ahora las copia Windows).
- Resolver conflictos de nombre dentro de FastCopy (ahora los resuelve Windows).
- Verificación también en copias (ahora solo se verifica al mover).
