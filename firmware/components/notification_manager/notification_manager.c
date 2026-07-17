#include "notification_manager.h"

#include "esp_log.h"

static const char* TAG = "notification_manager";

void notification_manager_trigger(const char* quelle, float value, float threshold) {
  // Platzhalter bis Versandweg entschieden ist (Pflichtenheft Abschnitt 12) -
  // WARNING-Log ist bereits sichtbar ueber die serielle Konsole/spaetere
  // WebSocket-Konsole (P5), auch ohne konkreten externen Versandweg.
  ESP_LOGW(TAG, "Schwellwert ueberschritten: %s = %.1f > %.1f", quelle, value, threshold);
}
