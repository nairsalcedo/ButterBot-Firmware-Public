#include "TalkRoutine.h"
#include <Statics/ApplicationStatics.h>
#include <Core/Application.h>
#include <Services/Audio/Audio.h>
#include <Util/ServiceLocator.h>
#include <Util/stdafx.h>
#include <Log/Log.h>
#include <cstring>
#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <string>
#include "Audio/AudioFrontend.h"
#include "Audio/MemoryAudioSource.h"
#include "Audio/MicInput.h"
#include "Audio/SpeechAudioGen.h"
#include "Audio/SpeechAudioSource.h"
#include "Audio/SpeechGen.h"
#include "Services/LEDControl.h"
#include "Util/PSRAMAllocator.h"
#include "Util/TalkSecrets.h"

DEFINE_LOG(TalkRoutine)

namespace {
	constexpr uint32_t SampleRate = 16000;
	constexpr uint32_t RecordMs = 5000;
	constexpr size_t RecordSamples = SampleRate * RecordMs / 1000;
	constexpr size_t MaxReplyBytes = SampleRate * 2 * 25; // 25 s de respuesta como máximo
	constexpr size_t MaxClipBytes = SampleRate * 2 * 4; // «sigo pensando»: 4 s como máximo
	constexpr uint32_t WiFiTimeoutMs = 15000;
	constexpr int HttpTimeoutMs = 120000; // leer Obsidian u Opus puede tardar casi un minuto
	constexpr uint32_t ThinkingEveryMs = 10000;

	constexpr EventBits_t GotIPBit = BIT0;

	// WiFi en modo estación, levantado solo durante la rutina y apagado al terminar.
	struct Link {
		EventGroupHandle_t events = nullptr;
		esp_netif_t* netif = nullptr;
		esp_event_handler_instance_t wifiHandler = nullptr;
		esp_event_handler_instance_t ipHandler = nullptr;
		bool createdLoop = false;
		bool started = false;

		bool start(){
			events = xEventGroupCreate();
			if(events == nullptr) return false;

			esp_netif_init();
			const esp_err_t loopErr = esp_event_loop_create_default();
			if(loopErr == ESP_OK){
				createdLoop = true;
			}else if(loopErr != ESP_ERR_INVALID_STATE){
				CMF_LOG(TalkRoutine, LogLevel::Error, "event loop: %s", esp_err_to_name(loopErr));
				return false;
			}

			netif = esp_netif_create_default_wifi_sta();

			esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, [](void*, esp_event_base_t, int32_t, void*){
				esp_wifi_connect(); // reintentar mientras dure la rutina
			}, this, &wifiHandler);

			esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, [](void* arg, esp_event_base_t, int32_t, void*){
				xEventGroupSetBits(static_cast<Link*>(arg)->events, GotIPBit);
			}, this, &ipHandler);

			const wifi_init_config_t initCfg = WIFI_INIT_CONFIG_DEFAULT();
			if(esp_wifi_init(&initCfg) != ESP_OK){
				CMF_LOG(TalkRoutine, LogLevel::Error, "esp_wifi_init failed");
				return false;
			}

			wifi_config_t cfg = {};
			strncpy(reinterpret_cast<char*>(cfg.sta.ssid), TALK_WIFI_SSID, sizeof(cfg.sta.ssid) - 1);
			strncpy(reinterpret_cast<char*>(cfg.sta.password), TALK_WIFI_PASS, sizeof(cfg.sta.password) - 1);
			cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

			esp_wifi_set_mode(WIFI_MODE_STA);
			esp_wifi_set_config(WIFI_IF_STA, &cfg);
			if(esp_wifi_start() != ESP_OK){
				CMF_LOG(TalkRoutine, LogLevel::Error, "esp_wifi_start failed");
				return false;
			}
			started = true;
			esp_wifi_connect();
			return true;
		}

		bool waitIP(uint32_t timeoutMs){
			if(events == nullptr) return false;
			const EventBits_t bits = xEventGroupWaitBits(events, GotIPBit, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeoutMs));
			return bits & GotIPBit;
		}

		~Link(){
			if(wifiHandler) esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifiHandler);
			if(ipHandler) esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ipHandler);
			if(started){
				esp_wifi_disconnect();
				esp_wifi_stop();
			}
			esp_wifi_deinit();
			if(netif){
				esp_wifi_clear_default_wifi_driver_and_handlers(netif);
				esp_netif_destroy(netif);
			}
			if(createdLoop) esp_event_loop_delete_default();
			if(events) vEventGroupDelete(events);
		}
	};

	void say(Audio* audio, const char* text, bool wait){
		auto source = std::make_unique<SpeechAudioSource>(SpeechGen::InputType::Text, std::string(text));
		audio->play(ServiceLocator::SpeechAudioGenInstance.get(), std::move(source));
		if(wait) audio->waitEnd(portMAX_DELAY);
	}

	// Hace la petición al servidor y guarda lo que regresa (PCM 16 kHz mono) en reply.
	// samples == nullptr → GET; si no, POST con el audio grabado.
	bool request(const char* url, const int16_t* samples, size_t count, PSRAMByteBuffer& reply, size_t maxBytes, int timeoutMs){
		esp_http_client_config_t cfg = {};
		cfg.url = url;
		cfg.method = samples ? HTTP_METHOD_POST : HTTP_METHOD_GET;
		cfg.timeout_ms = timeoutMs;
		cfg.buffer_size = 4096;

		esp_http_client_handle_t client = esp_http_client_init(&cfg);
		if(client == nullptr) return false;

		esp_http_client_set_header(client, "X-Token", TALK_TOKEN);
		if(samples) esp_http_client_set_header(client, "Content-Type", "application/octet-stream");

		const int bodyLen = samples ? (int) (count * sizeof(int16_t)) : 0;
		bool ok = false;

		if(esp_http_client_open(client, bodyLen) == ESP_OK){
			const char* body = reinterpret_cast<const char*>(samples);
			int sent = 0;
			while(sent < bodyLen){
				const int w = esp_http_client_write(client, body + sent, std::min(4096, bodyLen - sent));
				if(w <= 0) break;
				sent += w;
			}

			if(sent == bodyLen && esp_http_client_fetch_headers(client) >= 0){
				const int status = esp_http_client_get_status_code(client);
				CMF_LOG(TalkRoutine, LogLevel::Info, "HTTP %d", status);
				if(status == 200){
					reply.reserve(samples ? 64 * 1024 : 16 * 1024);
					char chunk[2048];
					while(reply.size() < maxBytes){
						const int r = esp_http_client_read(client, chunk, sizeof(chunk));
						if(r <= 0) break;
						reply.insert(reply.end(), chunk, chunk + r);
					}
					ok = !reply.empty();
				}
			}
		}else{
			CMF_LOG(TalkRoutine, LogLevel::Error, "No se pudo abrir la conexión al servidor");
		}

		esp_http_client_close(client);
		esp_http_client_cleanup(client);
		return ok;
	}

	// La petición principal corre en su propia tarea para que el robot pueda decir «sigo pensando» mientras espera.
	struct PostJob {
		const int16_t* samples;
		size_t count;
		PSRAMByteBuffer reply;
		bool ok = false;
		SemaphoreHandle_t done;
	};

	void postTask(void* arg){
		auto* job = static_cast<PostJob*>(arg);
		job->ok = request(TALK_SERVER_URL, job->samples, job->count, job->reply, MaxReplyBytes, HttpTimeoutMs);
		xSemaphoreGive(job->done);
		vTaskDelete(nullptr);
	}

	// TALK_SERVER_URL termina en /talk; el clip vive en /pensando del mismo servidor.
	std::string thinkingUrl(){
		std::string url = TALK_SERVER_URL;
		return url.substr(0, url.rfind('/')) + "/pensando";
	}
}

Routine::TickingState TalkRoutine::tick(float deltaTime){
	const Application* app = ApplicationStatics::getApplication();
	Audio* audio = app->getService<Audio>();

	if(audio == nullptr || !ServiceLocator::SpeechAudioGenInstance || !ServiceLocator::SpeechGenInstance || !ServiceLocator::MicInputInstance){
		CMF_LOG(TalkRoutine, LogLevel::Error, "Missing required service(s)");
		return TickingState::Done;
	}

	// El reconocimiento de comandos no debe estar leyendo el micrófono mientras grabamos.
	if(AudioFrontend* af = app->getService<AudioFrontend>()){
		af->setMode(AudioFrontend::Mode::Off);
	}

	// Prender el WiFi desde ya: se conecta mientras Nair habla.
	auto link = std::make_unique<Link>();
	const bool wifiStarted = link->start();

	say(audio, "Go ahead", true);

	auto* samples = static_cast<int16_t*>(heap_caps_malloc(RecordSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
	if(samples == nullptr){
		CMF_LOG(TalkRoutine, LogLevel::Error, "Sin memoria para grabar");
		return TickingState::Done;
	}

	if(ServiceLocator::LEDControlInstance) ServiceLocator::LEDControlInstance->startListen();
	size_t recorded = 0;
	while(recorded < RecordSamples){
		const size_t want = std::min<size_t>(2048, RecordSamples - recorded); // getMono lee 2048 cuadros por llamada; pedir menos descarta audio
		const int32_t got = ServiceLocator::MicInputInstance->getMono({ samples + recorded, want });
		if(got <= 0) break;
		recorded += (size_t) got;
	}
	if(ServiceLocator::LEDControlInstance) ServiceLocator::LEDControlInstance->stopListen();
	CMF_LOG(TalkRoutine, LogLevel::Info, "Grabadas %u muestras", (unsigned) recorded);

	say(audio, "Let me think", false);

	PostJob job{ samples, recorded };
	job.done = xSemaphoreCreateBinary();
	bool ok = false;
	if(job.done == nullptr){
		CMF_LOG(TalkRoutine, LogLevel::Error, "Sin memoria para el semáforo");
	}else if(wifiStarted && link->waitIP(WiFiTimeoutMs)){
		// Clip de «sigo pensando» en español (si falla, se usa la voz del robot en inglés).
		PSRAMByteBuffer thinking;
		request(thinkingUrl().c_str(), nullptr, 0, thinking, MaxClipBytes, 5000);

		if(xTaskCreate(postTask, "TalkPost", 8192, &job, 5, nullptr) == pdPASS){
			while(xSemaphoreTake(job.done, pdMS_TO_TICKS(ThinkingEveryMs)) != pdTRUE){
				audio->waitEnd(portMAX_DELAY);
				if(!thinking.empty()){
					PSRAMByteBuffer copy = thinking;
					audio->play(ServiceLocator::SpeechAudioGenInstance.get(), std::make_unique<MemoryAudioSource>(std::move(copy)));
				}else{
					say(audio, "Still thinking", false);
				}
			}
			ok = job.ok;
		}else{
			CMF_LOG(TalkRoutine, LogLevel::Error, "No se pudo crear la tarea de la petición");
		}
	}else{
		CMF_LOG(TalkRoutine, LogLevel::Error, "No hubo WiFi: ¿está prendido el hotspot?");
	}

	if(job.done) vSemaphoreDelete(job.done);
	heap_caps_free(samples);
	link.reset(); // apagar el WiFi antes de hablar

	audio->waitEnd(portMAX_DELAY);

	if(!ok){
		say(audio, "I cannot reach my brain. Is the laptop on?", true);
		return TickingState::Done;
	}

	audio->play(ServiceLocator::SpeechAudioGenInstance.get(), std::make_unique<MemoryAudioSource>(std::move(job.reply)));
	audio->waitEnd(portMAX_DELAY);

	return TickingState::Done;
}
