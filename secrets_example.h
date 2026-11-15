#pragma once
// Copia este archivo como secrets.h (en esta misma carpeta) y completa los datos.
// Mantén secrets.h privado: no lo compartas ni lo subas a ningún chat.

// Wi-Fi de 2,4 GHz. Deja WIFI_SSID vacío ("") para usar la placa solo en modo local.
#define WIFI_SSID "NOMBRE_WIFI_2_4_GHZ"
#define WIFI_PASSWORD "CONTRASENA_WIFI"

// Clave de la placa para el panel de ClassSense (16 a 119 letras, números, _ o -).
#define DEVICE_TOKEN "CAMBIAR_CLAVE_DE_16_A_119_CARACTERES"

// LED RGB: true = ánodo común (pata larga a 3V3), false = cátodo común (pata larga a GND).
#define RGB_COMMON_ANODE true

// Cambia a true solo después de revisar el cableado con la tabla del LEEME.
#define OUTPUTS_CONFIRMED false

// true: sin panel, la placa avisa por su cuenta con el sensor de ruido.
// false: sin panel, luces y sonido quedan apagados (comportamiento de la versión 1.0).
#define LOCAL_MODE true
