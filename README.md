# ClassSense Alerts 2.4 Plus · reacción a episodios y control desde el celular

Programa para ESP32 DevKit / NodeMCU ESP32-WROOM-32 de 30 o 38 pines. Sigue siendo compatible con el panel de ClassSense (misma conexión por IP y clave), y además funciona por su cuenta sin Wi-Fi.

Este paquete contiene el firmware completo: pantalla, ruido, RGB, buzzer y los tres pulsadores. `VERSION.json` identifica la versión y los hashes de sus archivos públicos. Extrae **toda la carpeta ClassSenseAlerts**, incluidos los archivos `.h`, antes de abrir `ClassSenseAlerts.ino`. No mezcles archivos de descargas anteriores.

**Plataforma de compilación:** ESP32 Dev Module, esp32 by Espressif Systems 3.3.12, Adafruit SSD1306 2.5.17 y Adafruit GFX 1.13.0. Los resultados de compilación se entregan por separado. Compilar y simular el protocolo no demuestra que el montaje físico funcione: comprueba sus componentes al conectarlo.

## Conectar por Wi-Fi

1. En el panel publicado abre Control de alertas y prepara el ZIP con el Wi-Fi de **2,4 GHz**, la clave de la placa y la polaridad correcta. Los datos se añaden en tu navegador.
2. Conecta laptop hospedadora, ESP32 y celular al mismo punto de acceso. En redes de invitados o universidades, el aislamiento entre clientes puede impedir la comunicación incluso con el mismo nombre de red. Un punto de acceso propio permite comprobarlo.
3. Carga el sketch desde Arduino IDE con **ESP32 Dev Module** y abre el monitor serie a **115200**. Debe aparecer `ClassSense Alerts 2.4 Plus`, la IP y el resultado de detección de la pantalla.
4. En el panel pulsa Buscar placa o introduce la IP mostrada. Usa la misma clave incluida en `secrets.h`, prueba conexión y guarda. No introduzcas la contraseña del panel en ese campo.
5. Conecta el celular desde Cámara y conexión y concede su permiso. Prueba RGB, buzzer y pulsadores con las acciones explícitas del panel. La actualización del motor no carga el firmware por USB: son dos instalaciones distintas.

La versión 2.4 conserva los pines y ajustes de la 2.3, renueva el mensaje de arranque y añade mensajes explícitos del patrón observado, confirmación de inicio del tono y un panel local accesible desde cualquier navegador de la red. No identifica por sí sola la atención: la cámara y el análisis pertenecen al hospedador.

## Reacción a los episodios de la cámara

En el motor 2.12.0, «Cada episodio confirmado» emite un tono una vez que el patrón cumple la persistencia configurada. No pita en cada fotograma ni antes de confirmar el episodio. El LED refleja el estado y la OLED muestra «PATRÓN CONFIRMADO» y el patrón (cabeza girada, inclinada, en mesa o celular en mano), incluso si estaba abierta la pantalla de información. La regla de reincidencia y su historial se conservan; también puedes elegir que el sonido espere esa regla.

El pulsador de silencio y el volumen cero se respetan. El panel indica si el firmware inició el tono o si la placa está silenciada. Esa confirmación del programa no es una medición del sonido físico. Los reintentos con el mismo identificador no vuelven a pitar ni duplican el contador.

## Usarla desde un celular u otro dispositivo por Wi-Fi

Puedes usar un router o el punto de acceso de **2,4 GHz** de un celular. Introduce el nombre y contraseña de esa red al preparar el ZIP y conecta también el hospedador a ella. Cuando aparezca la IP en la OLED, abre `http://IP_DE_LA_PLACA/` desde un celular, tablet o computadora de esa red. Introduce **la clave de la placa**: verás estado, mensaje, ruido y volumen, y podrás aplicar ajustes y probar cada salida. La clave permanece en memoria de esa página; no se pone en la URL ni se guarda en almacenamiento del navegador.

Las órdenes manuales solo se ejecutan al pulsar sus controles. Los ajustes hechos desde ese panel local se respetan como los de los pulsadores hasta que guardes un cambio desde ClassSense. Si el router o hotspot aísla a sus clientes, hay que usar una red que permita comunicarse entre ellos. El panel local no ejecuta detección de cámara; para ella sigue siendo necesario un hospedador activo.

## Conexiones

Une todos los GND en un mismo riel de la protoboard y todos los VCC en el riel de 3V3. Desconecta el USB antes de mover cables.

| Componente | Pin del módulo | ESP32 |
| --- | --- | --- |
| **OLED SSD1306 0,96" (I2C)** | VCC / GND | 3V3 / GND |
| | SDA | GPIO 21 |
| | SCL | GPIO 22 |
| **KY-038** | + / G | 3V3 / GND |
| | D0 | GPIO 34 |
| **Buzzer HW-508** | VCC / GND | 3V3 / GND |
| | S | GPIO 4 |
| **LED RGB (ánodo común)** | Pata larga (común) | 3V3 |
| | Rojo → 220 Ω | GPIO 25 |
| | Verde → 220 Ω | GPIO 26 |
| | Azul → 220 Ω | GPIO 27 |
| **Pulsador 1** (encender/apagar) | un lado / otro lado | GPIO 32 / GND |
| **Pulsador 2** (buzzer) | un lado / otro lado | GPIO 33 / GND |
| **Pulsador 3** (LED) | un lado / otro lado | GPIO 14 / GND |

Los pulsadores no necesitan resistencia: el programa activa la resistencia interna de la placa.

## Qué se puede ajustar (desde el panel o la placa)

- **Volumen del buzzer:** 10 niveles más silencio. El nivel 10 usa un ciclo de trabajo del 50 %. Los niveles inferiores usan una tabla PWM diseñada para cambios graduales. El volumen audible y el timbre dependen del buzzer y de su alimentación; no son una medición ni una garantía en decibelios. Un buzzer activo no responde como uno pasivo. `VOLUME_DUTY` contiene los valores de 0 a 512.
- **Pitido de arranque:** al encender, la placa da un pitido corto al volumen que tiene guardado: así compruebas al oído que el buzzer funciona. Si no suena, revisa el silencio (pulsador 2) y el cableado del pin S en GPIO 4.
- **Duración del tono:** se escribe en segundos, de 0,1 a 10 (por ejemplo 2,5). Se aplica a los episodios confirmados, las reincidencias, los avisos locales de ruido y la prueba del buzzer.
- **Sensibilidad del ruido:** 10 niveles. Cada nivel define qué porcentaje del tiempo debe haber actividad en el sensor para pasar a amarillo y para dar la alerta (nivel 1: alerta con el 80 %; nivel 5: 56 %; nivel 10: 26 %).
- **Colores:** cada estado (sin episodios, en evaluación, episodio, observación insuficiente y reposo de la placa) puede tener cualquier color, con 0 a 255 en cada canal. El panel también muestra un color libre en la placa mientras mueves los controles. Se guardan en la placa y sobreviven a un reinicio.
- **Brillo del LED:** 0 a 100 %. Los canales usan una curva gamma para que los tonos intermedios se vean parejos.

Volumen, sensibilidad, colores, brillo, silencio y LED se guardan en la placa.

## Pulsadores

| Pulsador | Pulsación corta | Mantener ~1 s |
| --- | --- | --- |
| **1** encender/apagar (GPIO 32) | Cambia entre la pantalla principal y la de información (IP, señal, sensibilidad, tiempo encendido) | Apaga la placa; con la placa apagada, púlsalo para encenderla |
| **2** buzzer (GPIO 33) | Apaga o reactiva el buzzer | Cambia el volumen: un nivel cada 0,6 s mientras lo mantienes, con pitido de muestra; sube hasta 10 y luego baja |
| **3** LED (GPIO 14) | Apaga o enciende el LED RGB | Cambia el color de reposo: verde, azul, cian, morado, blanco |

**Sobre «apagar»:** la placa entra en sueño profundo. Se apagan pantalla, LED, buzzer y Wi-Fi, y el contador de alertas vuelve a cero al encender. No corta la corriente: la luz roja de la placa y la del KY-038 siguen encendidas mientras haya USB.

Lo que cambias con los pulsadores prevalece hasta que cambies ese mismo valor desde el panel.

## Pantalla

- **Arriba:** nombre, modo (`PANEL` cuando recibe órdenes de ClassSense, `LOCAL` cuando trabaja sola), barras de Wi-Fi y altavoz con el volumen.
- **Centro:** estado en letras grandes. Parpadea invertido durante una alerta.
- **Ruido:** barra con la actividad del último segundo; la marca vertical indica el umbral de alerta de la sensibilidad elegida.
- **Abajo:** nivel de volumen (10 bloques) y número de alertas de ruido.
- Al cambiar el volumen, el buzzer o el LED aparece un recuadro grande durante 1,5 s. Tras un minuto sin actividad la pantalla se atenúa.

## Modos

**Modo panel.** El panel envía el estado y la placa muestra el color elegido para cada uno (por defecto verde, amarillo, rojo y azul; rojo intermitente si falla la cámara). El tono de reincidencia respeta el volumen y la duración elegidos. La placa guarda esa duración, de modo que los avisos del modo local también duran exactamente lo que configuraste (de 0,1 a 10 s).

**Modo local.** Sin Wi-Fi o sin órdenes del panel durante 3,5 s, la placa usa el KY-038: color de reposo en calma, el color de «en evaluación» con ruido y el de «episodio» parpadeando con un aviso sonoro si el ruido se sostiene unos 3 s. Cuenta una alerta y repite el aviso cada 8 s mientras siga.

Para volver al comportamiento de 1.0 (todo apagado sin panel), pon `LOCAL_MODE false` en `secrets.h`.

## Ajustar el KY-038

El programa usa la salida digital D0, que solo indica «hay sonido por encima del umbral del módulo». No graba ni transmite audio.

1. En silencio, gira el tornillo azul del módulo hasta que su segundo LED quede justo apagado. **Este tornillo es la sensibilidad real del micrófono**: decide qué volumen de sonido cuenta.
2. Habla o aplaude cerca: el LED del módulo debe parpadear y la barra «Ruido» debe subir.
3. La sensibilidad de 10 niveles del panel decide después cuánto ruido seguido hace falta para avisar.

Con el cableado actual no se puede medir el volumen en decibelios ni ajustar el umbral por software. Para eso hay que conectar la salida analógica A0 del módulo a un pin de entrada analógica (por ejemplo GPIO 35) y ampliar el programa.

## Carga del programa

1. Instala Arduino IDE y, en el Gestor de placas, **esp32 by Espressif Systems 3.x**.
2. En el Gestor de librerías instala **Adafruit SSD1306** y **Adafruit GFX Library**.
3. Copia `secrets.example.h` como `secrets.h` en esta carpeta y complétalo. Si ya tienes el `secrets.h` de una versión anterior, sirve tal cual.
4. Revisa el cableado con la tabla y cambia `OUTPUTS_CONFIRMED` a `true`. Sin eso, el LED y el buzzer quedan bloqueados y la pantalla muestra `BLOQUEADO`.
5. Selecciona **ESP32 Dev Module**, el puerto de la placa y carga. Si lo pide, mantén BOOT al empezar la carga.
6. La IP aparece en el monitor serie (115200 baudios) y en la pantalla de información (pulsación corta del pulsador 1).

Si la pantalla queda negra, cambia `OLED_ADDRESS` de `0x3C` a `0x3D`. Los ajustes guardados por las versiones 2.0 y 2.1 (volumen de 5 y 8 niveles) no se reutilizan: la 2.2 empieza con el volumen en 6 de 10.
