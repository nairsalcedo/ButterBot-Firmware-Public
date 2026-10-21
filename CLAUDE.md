# Butter Bot firmware — notas para Claude Code

Firmware del robot Butter Bot (CircuitMess). ESP32-S3, flash de 16 MB y PSRAM octal. Se compila con **ESP-IDF 5.5.3**.

## Reglas
- Nair escribe en español. El robot **solo habla y entiende inglés**, así que frases, comandos de voz y textos del robot van en inglés.
- **Nunca** correr `idf.py set-target` ni "ESP-IDF: Set Espressif Device Target": regenera `sdkconfig` y borra la configuración del robot.
- Puerto del robot en esta laptop: **COM3** (USB-SERIAL CH340).
- Compilar: `idf.py build`. Flashear: `idf.py -p COM3 flash`. Correr dentro de la **ESP-IDF Terminal**, que es donde existe `idf.py`.
- **Compilar SIEMPRE con `idf.py --no-ccache build`**: con ccache, el compilador truena (internal compiler error en esp-dsp). Sin ccache compila bien.
- El repo público no traía `main/src/BLE/Bonds.h` (`BLE::isBonded`); lo escribimos nosotros (20 oct).
- **Espacio:** factory agrandada a 8592k. Con el Bluetooth apagado quedan ~190 KB libres (0x2f3b0); con Bluetooth, ~22 KB.
- **Bluetooth apagado** (`main/src/Util/NairConfig.h`, `Nair::Bluetooth = false`): con Bluetooth, la coexistencia de radio obliga al WiFi a dormir entre paquetes (`WIFI_PS_NONE` aborta) y la plática tardaba ~45 s. Sin él se pierden la app del teléfono y el control remoto; los comandos de voz y las rutinas siguen.
- **Botón de la cámara:** 1 toque = plática con Claude (`TalkRoutine`); 2 toques = comandos de voz de fábrica; mantener = prender/apagar.
- **Plática con Claude** (`main/src/Routines/TalkRoutine.cpp`): WiFi persistente (IP fija 192.168.137.237, PSK precalculada, canal recordado); graba hasta que Nair se calla y manda el audio en IMA ADPCM, por HTTP chunked, mientras graba; la respuesta es texto en inglés que dice la voz del robot. Servidor: repo privado `nairsalcedo/butterbot-server`.
- **Secretos:** `main/src/Util/TalkSecrets.h` (WiFi, PSK, token) lo genera `butterbot-server/generar_secretos.py` y está en `.gitignore`. Nunca subirlo.
- **Pilas de tareas:** el hilo de la máquina de estados tiene 4 KB de pila; WiFi/HTTP desde ahí lo desbordan. La red va en una tarea con pila en PSRAM (que no debe tocar flash).
- **Logs:** `main.cpp` baja todo a WARN: para ver algo, `ESP_LOGW`. Para leerlos sin reiniciar el robot: prenderlo desconectado y luego conectar el USB (con DTR/RTS apagados).
- **Si la app quedó a medias** (flasheo interrumpido), el robot solo enciende mientras se mantiene el botón: mantener la cámara presionada todo el flasheo (`idf.py -p COM3 app-flash`).
- Flashear borra lo que tenga cargado. El firmware de fábrica se restaura con "Restore Firmware" en code.circuitmess.com, o con `esptool -c esp32s3 -b 921600 -p COM3 write_flash 0 ButterBot-vX.bin` (binario en Releases).
- Problema conocido (upstream issue #5): después de flashear, el robot puede quedar sin responder por el *power latch*. **Desconectar el USB** y luego prenderlo manteniendo el botón **4–5 s**; conectado, se queda colgado. **El botón de encendido es la cámara.**
- Remotes: `origin` = fork de Nair (nairsalcedo), `upstream` = CircuitMess. Los cambios van en una rama propia, nunca directo sobre `master` de upstream.
- Guías oficiales (en español) en el vault de Obsidian: `Personal/Butter Bot/` (Guía de programación 1–6, plan para Windows).
