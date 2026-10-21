#include "TalkRoutine.h"
#include <Statics/ApplicationStatics.h>
#include <Core/Application.h>
#include <Services/Audio/Audio.h>
#include <Util/ServiceLocator.h>
#include <Util/stdafx.h>
#include <Log/Log.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <string>
#include "Audio/AudioFrontend.h"
#include "Audio/MicInput.h"
#include "Audio/SpeechAudioGen.h"
#include "Audio/SpeechAudioSource.h"
#include "Audio/SpeechGen.h"
#include "Services/LEDControl.h"
#include "Util/NairConfig.h"
#include "Util/TalkSecrets.h"
#include "Pins.hpp"

DEFINE_LOG(TalkRoutine)

// main.cpp baja todos los logs a WARN, así que lo que queremos ver va con ESP_LOGW.
static const char* TAG = "TalkRoutine";

namespace {
	constexpr uint32_t SampleRate = 16000;
	constexpr size_t ChunkSamples = 2048; // getMono lee 2048 cuadros por llamada (128 ms); pedir menos descarta audio
	constexpr uint32_t MaxRecordMs = 7000;
	constexpr size_t MaxRecordSamples = SampleRate * MaxRecordMs / 1000;

	// Dejar de grabar cuando Nair deja de hablar (energía por bloque de 128 ms).
	// El micrófono capta bajito (pruebas reales: ruido 50-190, voz 400-780), así que:
	// - empezó a hablar: 2 bloques seguidos arriba de 1.8x el ruido del cuarto (medido antes de «Go ahead»);
	// - ya se calló: abajo del 35 % de su voz más fuerte (o de 1.5x el ruido). Contra el ruido solo, un cuarto
	//   con ruido variable nunca contaba como silencio y grababa los 7 s completos.
	constexpr uint32_t EndSilenceMs = 1000; // silencio después de hablar
	constexpr uint32_t NoSpeechGiveUpMs = 4000; // si no habla nada
	constexpr int SpeechChunksToStart = 2; // 256 ms seguidos de voz para contar que ya empezó
	constexpr float SpeechOverNoise = 1.8f;
	constexpr float SilenceOverNoise = 1.5f;
	constexpr float SilenceOfPeak = 0.35f;
	constexpr float MinNoiseRms = 30.0f;

	constexpr size_t MaxReplyChars = 1200; // la respuesta es texto en inglés (la voz del robot)
	constexpr uint32_t WiFiTimeoutMs = 15000;
	constexpr int ConnectTimeoutMs = 5000;
	constexpr int IoTimeoutMs = 1000; // corto, para revisar seguido si Nair canceló
	constexpr uint64_t RequestDeadlineMs = 120000; // leer Obsidian u Opus puede tardar casi un minuto
	constexpr uint32_t ThinkingEveryMs = 10000;
	constexpr size_t EncodeBlockSamples = 1024; // par: 2 muestras por byte

	constexpr EventBits_t GotIPBit = BIT0;

	uint64_t nowMs(){
		return esp_timer_get_time() / 1000;
	}

	// WiFi en modo estación. Se prende en la primera plática y se queda conectado: sin Bluetooth hay RAM de sobra,
	// y así las siguientes pláticas no esperan a conectarse (de paso el hotspot de Windows no se apaga solo).
	// Si se cae: durante una plática reconecta al momento; en reposo, cada 20 s.
	struct Link {
		EventGroupHandle_t events = nullptr;
		esp_netif_t* netif = nullptr;
		esp_timer_handle_t retryTimer = nullptr;
		std::atomic<bool> talking{ false };
		std::atomic<uint8_t> channel{ 0 }; // canal del hotspot de la última conexión: la siguiente no escanea todos

		static constexpr uint64_t IdleRetryUs = 20ULL * 1000 * 1000;

		bool start(){
			events = xEventGroupCreate();
			if(events == nullptr) return false;

			esp_netif_init();
			const esp_err_t loopErr = esp_event_loop_create_default();
			if(loopErr != ESP_OK && loopErr != ESP_ERR_INVALID_STATE){
				ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(loopErr));
				return false;
			}

			netif = esp_netif_create_default_wifi_sta();
			if(netif == nullptr) return false;

			// IP fija: el DHCP del hotspot de Windows tardaba segundos.
			esp_netif_dhcpc_stop(netif);
			esp_netif_ip_info_t ip = {};
			ip.ip.addr = esp_ip4addr_aton(TALK_STATIC_IP);
			ip.gw.addr = esp_ip4addr_aton(TALK_GATEWAY);
			ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");
			esp_netif_set_ip_info(netif, &ip);

			esp_timer_create_args_t timerArgs = {};
			timerArgs.callback = [](void*){ esp_wifi_connect(); };
			timerArgs.dispatch_method = ESP_TIMER_TASK;
			timerArgs.name = "TalkWiFi";
			esp_timer_create(&timerArgs, &retryTimer);

			esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, [](void* arg, esp_event_base_t, int32_t, void* data){
				static_cast<Link*>(arg)->channel = static_cast<wifi_event_sta_connected_t*>(data)->channel;
			}, this, nullptr);
			esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, [](void* arg, esp_event_base_t, int32_t, void* data){
				static_cast<Link*>(arg)->onDisconnected(static_cast<wifi_event_sta_disconnected_t*>(data)->reason);
			}, this, nullptr);
			esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, [](void* arg, esp_event_base_t, int32_t, void*){
				xEventGroupSetBits(static_cast<Link*>(arg)->events, GotIPBit);
			}, this, nullptr);

			const wifi_init_config_t initCfg = WIFI_INIT_CONFIG_DEFAULT();
			if(esp_wifi_init(&initCfg) != ESP_OK){
				ESP_LOGE(TAG, "esp_wifi_init falló");
				return false;
			}
			esp_wifi_set_storage(WIFI_STORAGE_RAM); // no escribir la configuración en flash
			esp_wifi_set_mode(WIFI_MODE_STA);
			applyConfig();
			if(esp_wifi_start() != ESP_OK){
				ESP_LOGE(TAG, "esp_wifi_start falló");
				return false;
			}
			esp_wifi_connect();
			return true;
		}

		void applyConfig(){
			wifi_config_t cfg = {};
			strncpy(reinterpret_cast<char*>(cfg.sta.ssid), TALK_WIFI_SSID, sizeof(cfg.sta.ssid) - 1);
			// La clave ya derivada (64 hex, PBKDF2 hecho en la laptop): el robot no la calcula y no intenta WPA3/SAE.
			static_assert(sizeof(TALK_WIFI_PSK) - 1 == sizeof(cfg.sta.password), "TALK_WIFI_PSK debe tener 64 caracteres hex");
			memcpy(cfg.sta.password, TALK_WIFI_PSK, sizeof(cfg.sta.password));
			cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
			cfg.sta.scan_method = WIFI_FAST_SCAN;
			cfg.sta.channel = channel;
			esp_wifi_set_config(WIFI_IF_STA, &cfg);
		}

		void onDisconnected(uint8_t reason){
			xEventGroupClearBits(events, GotIPBit);
			if(channel != 0 && (reason == WIFI_REASON_NO_AP_FOUND || reason == WIFI_REASON_BEACON_TIMEOUT)){
				channel = 0; // el hotspot pudo cambiar de canal: escanear todos
				applyConfig();
			}
			if(talking){
				esp_wifi_connect();
			}else{
				esp_timer_stop(retryTimer);
				esp_timer_start_once(retryTimer, IdleRetryUs);
			}
		}

		bool connected() const{
			return xEventGroupGetBits(events) & GotIPBit;
		}

		void beginTalk(){
			talking = true;
			esp_timer_stop(retryTimer);
			// Sin Bluetooth se puede quitar el ahorro de energía del WiFi: sin él no hay paquetes perdidos esperando
			// a que el robot despierte. Con Bluetooth prendido no se permite (aborta).
			if(!Nair::Bluetooth) esp_wifi_set_ps(WIFI_PS_NONE);
			if(!connected()) esp_wifi_connect();
		}

		void endTalk(){
			talking = false;
			esp_wifi_set_ps(WIFI_PS_MIN_MODEM); // en reposo, ahorrar pila
		}

		bool waitIP(uint32_t timeoutMs){
			const EventBits_t bits = xEventGroupWaitBits(events, GotIPBit, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeoutMs));
			return bits & GotIPBit;
		}
	};

	// Una sola conexión WiFi para siempre (hasta reiniciar). Si no se pudo levantar, no se reintenta.
	Link* wifiLink = nullptr;
	bool wifiLinkFailed = false;

	Link* getLink(){
		if(wifiLink == nullptr && !wifiLinkFailed){
			auto* l = new Link();
			if(l->start()){
				wifiLink = l;
			}else{
				wifiLinkFailed = true; // quedó a medias: no se puede volver a levantar sin reiniciar
				ESP_LOGE(TAG, "No se pudo prender el WiFi");
			}
		}
		return wifiLink;
	}

	// IMA ADPCM de 4 bits, un solo flujo continuo (predictor 0, índice 0 al inicio).
	// Debe coincidir con butterbot-server/adpcm.py.
	constexpr int16_t AdpcmStep[89] = {
		7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
		118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
		1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
		6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
		32767
	};
	constexpr int8_t AdpcmIndex[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

	struct AdpcmEncoder {
		int predicted = 0;
		int index = 0;

		uint8_t encode(int16_t sample){
			int step = AdpcmStep[index];
			int diff = sample - predicted;
			uint8_t nibble = 0;
			if(diff < 0){
				nibble = 8;
				diff = -diff;
			}
			int delta = step >> 3;
			if(diff >= step){
				nibble |= 4;
				diff -= step;
				delta += step;
			}
			step >>= 1;
			if(diff >= step){
				nibble |= 2;
				diff -= step;
				delta += step;
			}
			step >>= 1;
			if(diff >= step){
				nibble |= 1;
				delta += step;
			}
			predicted += (nibble & 8) ? -delta : delta;
			predicted = std::clamp(predicted, -32768, 32767);
			index = std::clamp(index + AdpcmIndex[nibble], 0, 88);
			return nibble;
		}
	};

	float rmsOf(const int16_t* s, size_t n){
		if(n == 0) return 0.0f;
		uint64_t acc = 0;
		for(size_t i = 0; i < n; ++i){
			acc += (int64_t) s[i] * s[i];
		}
		return sqrtf((float) acc / (float) n);
	}

	// Detector de voz muy simple: ¿ya habló?, ¿ya se calló?
	struct Vad {
		float noise = MinNoiseRms; // ruido del cuarto, medido antes de «Go ahead»
		float peak = 0.0f; // la voz más fuerte desde que empezó a hablar
		bool speaking = false;
		int speechRun = 0;
		uint32_t elapsedMs = 0;
		uint32_t silenceMs = 0;
		uint32_t speechStartMs = 0;

		// true = ya se puede dejar de grabar
		bool feed(const int16_t* s, size_t n){
			const float rms = rmsOf(s, n);
			const uint32_t ms = n * 1000 / SampleRate;
			elapsedMs += ms;

			if(speaking){
				peak = std::max(peak, rms);
				const float silence = std::max(noise * SilenceOverNoise, peak * SilenceOfPeak);
				silenceMs = rms < silence ? silenceMs + ms : 0;
				return silenceMs >= EndSilenceMs;
			}

			if(rms > noise * SpeechOverNoise){
				peak = std::max(peak, rms);
				if(++speechRun >= SpeechChunksToStart){
					speaking = true;
					speechStartMs = elapsedMs;
				}
			}else{
				speechRun = 0;
			}
			return elapsedMs >= NoSpeechGiveUpMs;
		}
	};

	void say(Audio* audio, const char* text, bool wait){
		auto source = std::make_unique<SpeechAudioSource>(SpeechGen::InputType::Text, std::string(text));
		audio->play(ServiceLocator::SpeechAudioGenInstance.get(), std::move(source));
		if(wait) audio->waitEnd(portMAX_DELAY);
	}

	// Botón de la cámara: 1 = presionado (igual que ShutdownService).
	bool buttonDown(){
		return gpio_get_level(static_cast<gpio_num_t>(PIN_BTN)) == 1;
	}

	// La red corre en su propia tarea: el hilo de la máquina de estados solo tiene 4 KB de pila y esp_http_client
	// lo desbordaba. La pila va en PSRAM porque la RAM interna no alcanza para WiFi + BLE + cámara; por eso esa
	// tarea no toca flash (el WiFi se prende y apaga desde la rutina, que sí tiene pila interna).
	// Solo esa tarea usa el cliente HTTP: para cancelar, la rutina prende `cancelled` y la tarea lo ve en ≤1 s.
	constexpr uint32_t NetStackSize = 12 * 1024;
	constexpr UBaseType_t NetPriority = 8;

	struct NetJob {
		Link* link = nullptr;
		bool wifiStarted = false;
		const int16_t* samples = nullptr;
		std::atomic<size_t> recordedCount{ 0 };
		std::atomic<bool> recordingDone{ false };
		uint64_t recordEndMs = 0; // se escribe antes de recordingDone
		std::atomic<bool> cancelled{ false };
		std::atomic<bool> finished{ false };
		std::string reply;
		bool ok = false;
		SemaphoreHandle_t done = nullptr;
	};

	bool expired(NetJob* job, uint64_t deadline){
		return job->cancelled || nowMs() > deadline;
	}

	// Escribe todo, reintentando cuando vence el timeout corto (esp_http_client_write regresa 0).
	bool writeAll(esp_http_client_handle_t client, const char* data, int len, NetJob* job, uint64_t deadline){
		int sent = 0;
		while(sent < len){
			if(expired(job, deadline)) return false;
			const int w = esp_http_client_write(client, data + sent, len - sent);
			if(w < 0) return false;
			sent += w;
		}
		return true;
	}

	// Un pedazo del cuerpo chunked; len == 0 cierra el cuerpo.
	bool writeChunk(esp_http_client_handle_t client, const uint8_t* data, size_t len, NetJob* job, uint64_t deadline){
		char head[16];
		const int n = snprintf(head, sizeof(head), "%x\r\n", (unsigned) len);
		return writeAll(client, head, n, job, deadline)
			   && (len == 0 || writeAll(client, reinterpret_cast<const char*>(data), (int) len, job, deadline))
			   && writeAll(client, "\r\n", 2, job, deadline);
	}

	// Manda el audio en ADPCM mientras se va grabando.
	bool streamAudio(esp_http_client_handle_t client, NetJob* job, uint64_t deadline){
		AdpcmEncoder encoder;
		uint8_t block[EncodeBlockSamples / 2];
		size_t sentSamples = 0;
		size_t sentBytes = 0;

		for(;;){
			if(expired(job, deadline)) return false;

			const bool finished = job->recordingDone.load(std::memory_order_acquire);
			const size_t available = job->recordedCount.load(std::memory_order_acquire);
			size_t n = std::min(available - sentSamples, EncodeBlockSamples);
			if(!finished) n &= ~static_cast<size_t>(1); // mientras graba, de dos en dos (2 muestras por byte)

			if(n == 0){
				if(finished) break;
				vTaskDelay(pdMS_TO_TICKS(20));
				continue;
			}

			size_t len = 0;
			for(size_t i = 0; i < n; i += 2){
				const uint8_t lo = encoder.encode(job->samples[sentSamples + i]);
				const uint8_t hi = (i + 1 < n) ? encoder.encode(job->samples[sentSamples + i + 1]) : 0;
				block[len++] = lo | (hi << 4);
			}
			if(!writeChunk(client, block, len, job, deadline)) return false;
			sentSamples += n;
			sentBytes += len;
		}

		if(!writeChunk(client, nullptr, 0, job, deadline)) return false;
		ESP_LOGW(TAG, "Audio enviado: %u muestras en %u bytes; terminó %u ms después de grabar",
				 (unsigned) sentSamples, (unsigned) sentBytes, (unsigned) (nowMs() - job->recordEndMs));
		return true;
	}

	bool readReply(esp_http_client_handle_t client, NetJob* job, uint64_t deadline){
		int64_t len;
		while((len = esp_http_client_fetch_headers(client)) == -ESP_ERR_HTTP_EAGAIN){
			if(expired(job, deadline)) return false;
		}
		if(len < 0) return false;

		const int status = esp_http_client_get_status_code(client);
		if(status != 200){
			ESP_LOGW(TAG, "HTTP %d", status);
			return false;
		}

		char chunk[256];
		while(job->reply.size() < MaxReplyChars){
			if(expired(job, deadline)) return false;
			const int r = esp_http_client_read(client, chunk, sizeof(chunk));
			if(r == -ESP_ERR_HTTP_EAGAIN) continue;
			if(r <= 0) break;
			job->reply.append(chunk, r);
		}
		ESP_LOGW(TAG, "Respuesta %u ms después de grabar: %s", (unsigned) (nowMs() - job->recordEndMs), job->reply.c_str());
		return !job->reply.empty();
	}

	bool talkToServer(NetJob* job){
		esp_http_client_config_t cfg = {};
		cfg.url = TALK_SERVER_URL "?voz=robot&fmt=adpcm";
		cfg.method = HTTP_METHOD_POST;
		cfg.timeout_ms = ConnectTimeoutMs;
		cfg.buffer_size = 1024;
		cfg.buffer_size_tx = 1024;

		esp_http_client_handle_t client = esp_http_client_init(&cfg);
		if(client == nullptr) return false;

		esp_http_client_set_header(client, "X-Token", TALK_TOKEN);
		esp_http_client_set_header(client, "Content-Type", "audio/x-ima-adpcm");

		const uint64_t deadline = nowMs() + RequestDeadlineMs;
		bool ok = false;
		if(esp_http_client_open(client, -1) == ESP_OK){ // -1 = cuerpo chunked, se manda mientras Nair habla
			esp_http_client_set_timeout_ms(client, IoTimeoutMs);
			ok = streamAudio(client, job, deadline) && readReply(client, job, deadline);
		}else{
			ESP_LOGE(TAG, "No se pudo conectar al servidor");
		}

		esp_http_client_close(client);
		esp_http_client_cleanup(client);
		return ok;
	}

	void netTask(void* arg){
		auto* job = static_cast<NetJob*>(arg);

		// El WiFi se conecta mientras dice «Go ahead» y Nair empieza a hablar.
		bool hasIP = false;
		const uint64_t ipDeadline = nowMs() + WiFiTimeoutMs;
		while(job->wifiStarted && !job->cancelled && nowMs() < ipDeadline){
			if(job->link->waitIP(200)){
				hasIP = true;
				break;
			}
		}
		ESP_LOGW(TAG, "WiFi: %s", hasIP ? "con IP" : "sin IP");

		if(hasIP && !job->cancelled){
			job->ok = talkToServer(job);
		}

		job->finished = true;
		xSemaphoreGive(job->done);
		vTaskSuspend(nullptr); // una tarea con pila en PSRAM no se puede borrar sola: la borra la rutina
	}

	void deleteNetTask(TaskHandle_t task){
		while(eTaskGetState(task) != eSuspended){
			vTaskDelay(pdMS_TO_TICKS(5));
		}
		vTaskDeleteWithCaps(task);
	}
}

Routine::TickingState TalkRoutine::tick(float deltaTime){
	const Application* app = ApplicationStatics::getApplication();
	Audio* audio = app->getService<Audio>();

	if(audio == nullptr || !ServiceLocator::SpeechAudioGenInstance || !ServiceLocator::SpeechGenInstance || !ServiceLocator::MicInputInstance){
		ESP_LOGE(TAG, "Faltan servicios de audio");
		return TickingState::Done;
	}

	// El reconocimiento de comandos no debe estar leyendo el micrófono mientras grabamos.
	if(AudioFrontend* af = app->getService<AudioFrontend>()){
		af->setMode(AudioFrontend::Mode::Off);
	}

	// El WiFi se prende la primera vez, en este hilo (pila interna: el arranque del WiFi lee la flash), y se queda.
	Link* net = getLink();
	if(net) net->beginTalk();

	auto* samples = static_cast<int16_t*>(heap_caps_malloc(MaxRecordSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
	auto job = std::make_unique<NetJob>();
	job->link = net;
	job->wifiStarted = net != nullptr;
	job->samples = samples;
	job->done = xSemaphoreCreateBinary();

	TaskHandle_t netHandle = nullptr;
	const bool netStarted = samples && job->done &&
							xTaskCreateWithCaps(netTask, "TalkNet", NetStackSize, job.get(), NetPriority, &netHandle, MALLOC_CAP_SPIRAM) == pdPASS;
	if(!netStarted){
		ESP_LOGE(TAG, "Sin memoria para la plática");
		if(job->done) vSemaphoreDelete(job->done);
		heap_caps_free(samples);
		if(net) net->endTalk();
		say(audio, "I cannot reach my brain. Is the laptop on?", true);
		return TickingState::Done;
	}

	// Ruido del cuarto antes de hablar: el más callado de dos bloques (el primero puede traer audio viejo del búfer).
	Vad vad;
	{
		int16_t* probe = samples; // se sobrescribe al grabar
		ServiceLocator::MicInputInstance->getMono({ probe, ChunkSamples });
		const float first = rmsOf(probe, ChunkSamples);
		ServiceLocator::MicInputInstance->getMono({ probe, ChunkSamples });
		vad.noise = std::max(MinNoiseRms, std::min(first, rmsOf(probe, ChunkSamples)));
	}

	say(audio, "Go ahead", true);

	// Grabar hasta que Nair se calle (o 7 s). La tarea de red lo va mandando mientras tanto.
	if(ServiceLocator::LEDControlInstance) ServiceLocator::LEDControlInstance->startListen();
	size_t recorded = 0;
	bool wasDown = buttonDown();
	bool firstChunk = true;
	while(recorded < MaxRecordSamples && !job->finished){
		const size_t want = std::min(ChunkSamples, MaxRecordSamples - recorded);
		const int32_t got = ServiceLocator::MicInputInstance->getMono({ samples + recorded, want });
		if(got <= 0) break;
		const size_t n = std::min((size_t) got, want); // getMono puede contar más de lo que escribió

		// El primer bloque puede traer el final de «Go ahead»: se manda, pero no cuenta para el detector.
		const bool stop = !firstChunk && vad.feed(samples + recorded, n);
		firstChunk = false;

		recorded += n;
		job->recordedCount.store(recorded, std::memory_order_release);

		const bool down = buttonDown();
		if(down && !wasDown){
			job->cancelled = true;
			break;
		}
		wasDown = down;
		if(stop) break;
	}
	if(ServiceLocator::LEDControlInstance) ServiceLocator::LEDControlInstance->stopListen();
	job->recordEndMs = nowMs();
	job->recordingDone.store(true, std::memory_order_release);
	ESP_LOGW(TAG, "Grabé %u ms (ruido %d, pico %d, voz desde %u ms: %s)",
			 (unsigned) (recorded * 1000 / SampleRate), (int) vad.noise, (int) vad.peak, (unsigned) vad.speechStartMs, vad.speaking ? "sí" : "no");

	if(!job->cancelled) say(audio, "Let me think", false);

	// Esperar la respuesta: «Still thinking» cada 10 s; un toque a la cámara cancela.
	uint32_t waitedMs = 0;
	while(xSemaphoreTake(job->done, pdMS_TO_TICKS(100)) != pdTRUE){
		const bool down = buttonDown();
		if(down && !wasDown && !job->cancelled){
			ESP_LOGW(TAG, "Cancelado con la cámara");
			job->cancelled = true;
		}
		wasDown = down;

		waitedMs += 100;
		if(!job->cancelled && waitedMs % ThinkingEveryMs == 0){
			audio->waitEnd(portMAX_DELAY);
			say(audio, "Still thinking", false);
		}
	}

	deleteNetTask(netHandle);
	vSemaphoreDelete(job->done);
	heap_caps_free(samples);
	if(net) net->endTalk(); // el WiFi se queda conectado, con ahorro de energía

	audio->waitEnd(portMAX_DELAY);

	if(job->cancelled){
		say(audio, "Never mind", true);
	}else if(!job->ok){
		say(audio, "I cannot reach my brain. Is the laptop on?", true);
	}else{
		say(audio, job->reply.c_str(), true);
	}

	return TickingState::Done;
}
