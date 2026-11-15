#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Used by the real sketch and by native C++ simulations. No GPIO, network or Arduino API.
class ReactionLogic {
 public:
  enum Kind : uint8_t { State=0, Posture=1, Recurrence=2, CameraFault=3 };
  enum Pattern : uint8_t { Turned=1, Inclined=2, Desk=4, Phone=8 };
  struct Result { bool fresh; bool sound; };
  Result accept(const char* id, Kind kind, uint8_t patterns, uint16_t duration,
                bool ready, bool muted, uint8_t volume) {
    if (!id || !*id || strlen(id)>119 || kind==State) return {false,false};
    for (const auto& old : ids_) if (strcmp(old,id)==0) return {false,false};
    strncpy(ids_[cursor_++ % 32],id,119);
    lastKind_=kind; lastPatterns_=patterns;
    if (kind==Posture) ++postures_;
    return {true, ready && !muted && volume>0 && duration>=100 && duration<=10000};
  }
  static uint8_t pattern(const char* name) {
    if (!name) return 0;
    if (strcmp(name,"cabeza_girada")==0) return Turned;
    if (strcmp(name,"cabeza_inclinada")==0) return Inclined;
    if (strcmp(name,"cabeza_en_mesa")==0) return Desk;
    if (strcmp(name,"celular_en_mano")==0) return Phone;
    return 0;
  }
  static const char* patternText(uint8_t value) {
    if (value & Desk) return "CABEZA EN MESA";
    if (value & Phone) return "CELULAR EN MANO";
    if (value & Inclined) return "CABEZA INCLINADA";
    if (value & Turned) return "CABEZA GIRADA";
    return "REVISA LA POSTURA";
  }
  static const char* title(Kind kind) {
    return kind==CameraFault?"FALLO DE CAMARA":kind==Recurrence?"REINCIDENCIA":"PATRON CONFIRMADO";
  }
  const char* message() const { return lastKind_==CameraFault?"REVISA LA CONEXION":patternText(lastPatterns_); }
  const char* heading() const { return title(lastKind_); }
  uint32_t postureCount() const { return postures_; }
 private:
  char ids_[32][120] = {};
  uint8_t cursor_=0, lastPatterns_=0;
  Kind lastKind_=State;
  uint32_t postures_=0;
};
