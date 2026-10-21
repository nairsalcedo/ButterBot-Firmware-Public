#ifndef BUTTERBOT_FIRMWARE_NAIRCONFIG_H
#define BUTTERBOT_FIRMWARE_NAIRCONFIG_H

// Ajustes del Butter Bot de Nair.
namespace Nair {
	// Bluetooth apagado. Con Bluetooth prendido el WiFi está obligado a dormir entre paquetes (coexistencia de
	// radio), pierde paquetes y la plática con Claude tardaba ~45 s; además ocupa RAM interna que el WiFi necesita.
	// Sin Bluetooth se pierden la app del teléfono y el control remoto (los comandos de voz y las rutinas siguen).
	// Para regresarlo: true, compilar y flashear.
	constexpr bool Bluetooth = false;
}

#endif //BUTTERBOT_FIRMWARE_NAIRCONFIG_H
