# Butter Bot firmware — notas para Claude Code

Firmware del robot Butter Bot (CircuitMess). ESP32-S3, flash de 16 MB y PSRAM octal. Se compila con **ESP-IDF 5.5.3**.

## Reglas
- Nair escribe en español. El robot **solo habla y entiende inglés**, así que frases, comandos de voz y textos del robot van en inglés.
- **Nunca** correr `idf.py set-target` ni "ESP-IDF: Set Espressif Device Target": regenera `sdkconfig` y borra la configuración del robot.
- Puerto del robot en esta laptop: **COM3** (USB-SERIAL CH340).
- Compilar: `idf.py build`. Flashear: `idf.py -p COM3 flash`. Correr dentro de la **ESP-IDF Terminal**, que es donde existe `idf.py`.
- **Compilar SIEMPRE con `idf.py --no-ccache build`**: con ccache, el compilador truena (internal compiler error en esp-dsp). Sin ccache compila bien.
- El repo público no traía `main/src/BLE/Bonds.h` (`BLE::isBonded`); lo escribimos nosotros (20 oct).
- **Espacio:** la partición de la app (factory, 8432k) queda con ~28 KB libres (0x7260). Frases y chistes caben; funciones grandes no, sin quitar algo.
- Flashear borra lo que tenga cargado. El firmware de fábrica se restaura con "Restore Firmware" en code.circuitmess.com, o con `esptool -c esp32s3 -b 921600 -p COM3 write_flash 0 ButterBot-vX.bin` (binario en Releases).
- Problema conocido (upstream issue #5): después de flashear, el robot puede quedar sin responder por el *power latch*. **Desconectar el USB** y luego prenderlo manteniendo el botón **4–5 s**; conectado, se queda colgado. **El botón de encendido es la cámara.**
- Remotes: `origin` = fork de Nair (nairsalcedo), `upstream` = CircuitMess. Los cambios van en una rama propia, nunca directo sobre `master` de upstream.
- Guías oficiales (en español) en el vault de Obsidian: `Personal/Butter Bot/` (Guía de programación 1–6, plan para Windows).
