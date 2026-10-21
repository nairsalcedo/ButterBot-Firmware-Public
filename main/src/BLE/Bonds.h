#ifndef BUTTERBOT_FIRMWARE_BONDS_H
#define BUTTERBOT_FIRMWARE_BONDS_H

// Falta en el repo público de CircuitMess: Client.cpp y ConMan.cpp lo incluyen.
// Devuelve true si la dirección está en la lista de dispositivos emparejados (bonded) de Bluedroid.

#include <cstdlib>
#include <cstring>

#include "esp_gap_ble_api.h"

namespace BLE {
	inline bool isBonded(const esp_bd_addr_t addr){
		if(addr == nullptr){
			return false;
		}

		const int count = esp_ble_get_bond_device_num();
		if(count <= 0){
			return false;
		}

		auto* devices = static_cast<esp_ble_bond_dev_t*>(std::malloc(sizeof(esp_ble_bond_dev_t) * count));
		if(devices == nullptr){
			return false;
		}

		int deviceCount = count;
		const esp_err_t result = esp_ble_get_bond_device_list(&deviceCount, devices);

		bool bonded = false;
		if(result == ESP_OK && deviceCount >= 0 && deviceCount <= count){
			for(int i = 0; i < deviceCount; i++){
				if(std::memcmp(addr, devices[i].bd_addr, ESP_BD_ADDR_LEN) == 0){
					bonded = true;
					break;
				}
			}
		}

		std::free(devices);
		return bonded;
	}
}

#endif //BUTTERBOT_FIRMWARE_BONDS_H
