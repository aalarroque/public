/*
 * Control de cinta transportadora con Arduino
 * -------------------------------------------
 * Hardware previsto (Arduino Uno/Nano):
 *   - Motor DC de la cinta a través de un driver tipo L298N / BTS7960 / L293D
 *   - Pulsador START (NA) y pulsador STOP (NA)
 *   - Seta de emergencia (contacto NC, fail-safe: si se corta el cable, para)
 *   - Sensor de presencia al final de la cinta (IR / fotoeléctrico, activo en LOW)
 *   - Potenciómetro para ajustar la velocidad
 *   - LED verde (en marcha), LED rojo (parada/fallo) y zumbador
 *
 * Funciones:
 *   - Arranque y parada suaves (rampa de aceleración)
 *   - Parada de emergencia con rearme manual (comando "reset" o START tras soltar la seta)
 *   - Contador de piezas detectadas por el sensor
 *   - Dos modos: CONTINUO (cuenta piezas sin parar) y PAUSA (para la cinta
 *     cuando llega una pieza y reanuda al retirarla o tras un tiempo)
 *   - Control y monitorización por el Monitor Serie (115200 baudios)
 *
 * Comandos por Serie (terminar con salto de línea):
 *   start | stop | reset | estado | contador0
 *   vel <0-100>     -> velocidad fija en %, "vel pot" vuelve al potenciómetro
 *   modo continuo | modo pausa
 *   dir adelante | dir atras   (solo con la cinta parada)
 */

// ---------------- Pines ----------------
const uint8_t PIN_BTN_START = 2;
const uint8_t PIN_BTN_STOP  = 3;
const uint8_t PIN_ESTOP     = 4;   // NC a GND: LOW = normal, HIGH = emergencia
const uint8_t PIN_SENSOR    = 5;   // LOW = pieza detectada
const uint8_t PIN_MOTOR_IN1 = 7;
const uint8_t PIN_MOTOR_IN2 = 8;
const uint8_t PIN_MOTOR_PWM = 9;   // ENA del driver (debe ser pin PWM)
const uint8_t PIN_LED_VERDE = 10;
const uint8_t PIN_LED_ROJO  = 11;
const uint8_t PIN_ZUMBADOR  = 12;
const uint8_t PIN_POT       = A0;

// ---------------- Parámetros ----------------
const uint8_t  PWM_MINIMO          = 60;    // por debajo el motor no vence la inercia
const uint8_t  PASO_RAMPA          = 5;     // incremento de PWM por paso de rampa
const uint16_t INTERVALO_RAMPA_MS  = 20;    // 255/5*20 ms ~ 1 s de 0 a máximo
const uint16_t ANTIRREBOTE_MS      = 30;
const uint32_t PAUSA_MAX_MS        = 5000;  // modo PAUSA: reanuda tras este tiempo aunque siga la pieza (0 = esperar siempre)
const uint32_t INTERVALO_ESTADO_MS = 2000;  // cada cuánto se imprime el estado en marcha

// ---------------- Tipos ----------------
enum Estado { PARADA, EN_MARCHA, PAUSADA, EMERGENCIA };
enum Modo   { CONTINUO, PAUSA };

struct Entrada {
  uint8_t  pin;
  bool     estable;      // último valor estable leído
  bool     ultimaLect;   // última lectura cruda
  uint32_t tCambio;
};

// ---------------- Variables ----------------
Estado   estado        = PARADA;
Modo     modo          = CONTINUO;
bool     haciaAdelante = true;
int      velFijaPct    = -1;     // -1 = usar potenciómetro
uint8_t  pwmActual     = 0;
uint8_t  pwmObjetivo   = 0;
uint32_t tRampa        = 0;
uint32_t tPausa        = 0;
uint32_t tEstado       = 0;
uint32_t contadorPiezas = 0;

Entrada btnStart = {PIN_BTN_START, HIGH, HIGH, 0};
Entrada btnStop  = {PIN_BTN_STOP,  HIGH, HIGH, 0};
Entrada sensor   = {PIN_SENSOR,    HIGH, HIGH, 0};

String bufferSerie;

// ---------------- Utilidades ----------------

// Devuelve true solo en el flanco de bajada (HIGH -> LOW) ya filtrado.
bool flancoBajada(Entrada &e) {
  bool lectura = digitalRead(e.pin);
  uint32_t ahora = millis();
  if (lectura != e.ultimaLect) {
    e.ultimaLect = lectura;
    e.tCambio = ahora;
  }
  if (ahora - e.tCambio >= ANTIRREBOTE_MS && lectura != e.estable) {
    e.estable = lectura;
    return e.estable == LOW;
  }
  return false;
}

bool emergenciaActiva() {
  return digitalRead(PIN_ESTOP) == HIGH;
}

const char *nombreEstado(Estado e) {
  switch (e) {
    case PARADA:     return "PARADA";
    case EN_MARCHA:  return "EN MARCHA";
    case PAUSADA:    return "PAUSADA";
    case EMERGENCIA: return "EMERGENCIA";
  }
  return "?";
}

void pitido(uint16_t ms) {
  digitalWrite(PIN_ZUMBADOR, HIGH);
  delay(ms);
  digitalWrite(PIN_ZUMBADOR, LOW);
}

uint8_t calcularPwmDeseado() {
  int pct = velFijaPct >= 0 ? velFijaPct : map(analogRead(PIN_POT), 0, 1023, 0, 100);
  if (pct <= 0) return 0;
  return map(pct, 1, 100, PWM_MINIMO, 255);
}

void aplicarDireccion() {
  digitalWrite(PIN_MOTOR_IN1, haciaAdelante ? HIGH : LOW);
  digitalWrite(PIN_MOTOR_IN2, haciaAdelante ? LOW : HIGH);
}

// Corta el motor al instante (emergencia).
void cortarMotor() {
  pwmActual = 0;
  pwmObjetivo = 0;
  analogWrite(PIN_MOTOR_PWM, 0);
  digitalWrite(PIN_MOTOR_IN1, LOW);
  digitalWrite(PIN_MOTOR_IN2, LOW);
}

void actualizarRampa() {
  uint32_t ahora = millis();
  if (ahora - tRampa < INTERVALO_RAMPA_MS) return;
  tRampa = ahora;

  if (pwmActual < pwmObjetivo) {
    // Arrancar directamente desde el mínimo para no quedarse "zumbando" parado
    pwmActual = max<int>(pwmActual + PASO_RAMPA, PWM_MINIMO);
    if (pwmActual > pwmObjetivo) pwmActual = pwmObjetivo;
  } else if (pwmActual > pwmObjetivo) {
    pwmActual = (pwmActual - pwmObjetivo > PASO_RAMPA) ? pwmActual - PASO_RAMPA : pwmObjetivo;
    if (pwmActual < PWM_MINIMO) pwmActual = pwmObjetivo;
  }
  analogWrite(PIN_MOTOR_PWM, pwmActual);
}

void actualizarLeds() {
  bool parpadeo = (millis() / 250) % 2;
  switch (estado) {
    case EN_MARCHA:
      digitalWrite(PIN_LED_VERDE, HIGH);
      digitalWrite(PIN_LED_ROJO, LOW);
      break;
    case PAUSADA:
      digitalWrite(PIN_LED_VERDE, parpadeo);
      digitalWrite(PIN_LED_ROJO, LOW);
      break;
    case PARADA:
      digitalWrite(PIN_LED_VERDE, LOW);
      digitalWrite(PIN_LED_ROJO, HIGH);
      break;
    case EMERGENCIA:
      digitalWrite(PIN_LED_VERDE, LOW);
      digitalWrite(PIN_LED_ROJO, parpadeo);
      break;
  }
}

void imprimirEstado() {
  Serial.print(F("Estado: "));    Serial.print(nombreEstado(estado));
  Serial.print(F(" | Modo: "));   Serial.print(modo == CONTINUO ? F("CONTINUO") : F("PAUSA"));
  Serial.print(F(" | Dir: "));    Serial.print(haciaAdelante ? F("adelante") : F("atras"));
  Serial.print(F(" | PWM: "));    Serial.print(pwmActual);
  Serial.print('/');              Serial.print(pwmObjetivo);
  Serial.print(F(" | Vel: "));
  if (velFijaPct >= 0) { Serial.print(velFijaPct); Serial.print(F("% fija")); }
  else                 { Serial.print(F("potenciometro")); }
  Serial.print(F(" | Piezas: ")); Serial.println(contadorPiezas);
}

// ---------------- Transiciones ----------------

void cambiarEstado(Estado nuevo) {
  if (nuevo == estado) return;
  estado = nuevo;
  Serial.print(F(">> "));
  Serial.println(nombreEstado(estado));
}

void arrancar() {
  if (estado == EMERGENCIA) {
    Serial.println(F("No se puede arrancar: emergencia activa. Use 'reset'."));
    return;
  }
  if (estado == EN_MARCHA) return;
  aplicarDireccion();
  pitido(150);  // aviso acústico antes de mover la cinta
  cambiarEstado(EN_MARCHA);
}

void parar() {
  if (estado == EMERGENCIA) return;
  pwmObjetivo = 0;  // la rampa decelera hasta 0
  cambiarEstado(PARADA);
}

void entrarEmergencia() {
  cortarMotor();
  cambiarEstado(EMERGENCIA);
  pitido(500);
}

void rearmar() {
  if (estado != EMERGENCIA) return;
  if (emergenciaActiva()) {
    Serial.println(F("Libere la seta de emergencia antes de rearmar."));
    return;
  }
  cambiarEstado(PARADA);  // tras rearmar queda parada; hay que dar START
}

// ---------------- Comandos serie ----------------

void procesarComando(String cmd) {
  cmd.trim();
  cmd.toLowerCase();
  if (cmd.length() == 0) return;

  if (cmd == "start")            arrancar();
  else if (cmd == "stop")        parar();
  else if (cmd == "reset")       rearmar();
  else if (cmd == "estado")      imprimirEstado();
  else if (cmd == "contador0")   { contadorPiezas = 0; Serial.println(F("Contador a 0")); }
  else if (cmd == "modo continuo") { modo = CONTINUO; Serial.println(F("Modo CONTINUO")); }
  else if (cmd == "modo pausa")    { modo = PAUSA;    Serial.println(F("Modo PAUSA")); }
  else if (cmd.startsWith("dir ")) {
    if (estado == EN_MARCHA || estado == PAUSADA || pwmActual > 0) {
      Serial.println(F("Pare la cinta antes de cambiar de direccion."));
    } else {
      haciaAdelante = (cmd == "dir adelante");
      Serial.println(haciaAdelante ? F("Direccion: adelante") : F("Direccion: atras"));
    }
  }
  else if (cmd == "vel pot") { velFijaPct = -1; Serial.println(F("Velocidad por potenciometro")); }
  else if (cmd.startsWith("vel ")) {
    velFijaPct = constrain((int)cmd.substring(4).toInt(), 0, 100);
    Serial.print(F("Velocidad fija: ")); Serial.print(velFijaPct); Serial.println('%');
  }
  else {
    Serial.print(F("Comando desconocido: ")); Serial.println(cmd);
  }
}

void leerSerie() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      procesarComando(bufferSerie);
      bufferSerie = "";
    } else if (bufferSerie.length() < 32) {
      bufferSerie += c;
    }
  }
}

// ---------------- Programa principal ----------------

void setup() {
  pinMode(PIN_BTN_START, INPUT_PULLUP);
  pinMode(PIN_BTN_STOP,  INPUT_PULLUP);
  pinMode(PIN_ESTOP,     INPUT_PULLUP);
  pinMode(PIN_SENSOR,    INPUT_PULLUP);
  pinMode(PIN_MOTOR_IN1, OUTPUT);
  pinMode(PIN_MOTOR_IN2, OUTPUT);
  pinMode(PIN_MOTOR_PWM, OUTPUT);
  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_ROJO,  OUTPUT);
  pinMode(PIN_ZUMBADOR,  OUTPUT);
  cortarMotor();

  Serial.begin(115200);
  bufferSerie.reserve(32);
  Serial.println(F("Cinta transportadora lista. Escriba 'estado' para ver la configuracion."));

  if (emergenciaActiva()) entrarEmergencia();
}

void loop() {
  // 1) La emergencia tiene prioridad absoluta
  if (emergenciaActiva() && estado != EMERGENCIA) entrarEmergencia();

  // 2) Entradas físicas
  bool start = flancoBajada(btnStart);
  bool stop  = flancoBajada(btnStop);
  bool pieza = flancoBajada(sensor);

  if (stop) parar();  // STOP tiene prioridad sobre START
  else if (start) {
    if (estado == EMERGENCIA) rearmar();
    else arrancar();
  }

  leerSerie();

  // 3) Lógica según estado
  switch (estado) {
    case EN_MARCHA:
      pwmObjetivo = calcularPwmDeseado();
      if (pieza) {
        contadorPiezas++;
        Serial.print(F("Pieza detectada. Total: "));
        Serial.println(contadorPiezas);
        if (modo == PAUSA) {
          tPausa = millis();
          pwmObjetivo = 0;
          cambiarEstado(PAUSADA);
        }
      }
      break;

    case PAUSADA: {
      pwmObjetivo = 0;
      bool piezaRetirada = (sensor.estable == HIGH);
      bool tiempoAgotado = PAUSA_MAX_MS > 0 && millis() - tPausa >= PAUSA_MAX_MS;
      if (piezaRetirada || tiempoAgotado) cambiarEstado(EN_MARCHA);
      break;
    }

    case PARADA:
      pwmObjetivo = 0;
      break;

    case EMERGENCIA:
      cortarMotor();
      break;
  }

  if (estado != EMERGENCIA) actualizarRampa();
  actualizarLeds();

  // 4) Telemetría periódica
  if (estado == EN_MARCHA && millis() - tEstado >= INTERVALO_ESTADO_MS) {
    tEstado = millis();
    imprimirEstado();
  }
}
