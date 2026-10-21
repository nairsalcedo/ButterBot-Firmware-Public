#ifndef BUTTERBOT_FIRMWARE_TALKROUTINE_H
#define BUTTERBOT_FIRMWARE_TALKROUTINE_H

#include "Routine.h"

/**
 * "Let's talk": graba unos segundos del micrófono, los manda por WiFi al servidor de la laptop de Nair
 * (Whisper → Claude → voz en español) y reproduce el audio que regresa.
 * La configuración (WiFi, URL y token) vive en Util/TalkSecrets.h, que no se sube a git.
 */
class TalkRoutine : public Routine {
public:
	using Routine::Routine;

	TickingState tick(float deltaTime) override;
};

#endif //BUTTERBOT_FIRMWARE_TALKROUTINE_H
