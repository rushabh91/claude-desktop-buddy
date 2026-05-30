#pragma once
#include <stdint.h>
#include <stddef.h>

// Nordic UART Service-compatible BLE bridge. Clients (browser Web
// Bluetooth, noble, etc.) subscribe to NUS to talk to the Stick exactly
// like a serial port.
//
// Service UUID  6e400001-b5a3-f393-e0a9-e50e24dcca9e
// RX char       6e400002-b5a3-f393-e0a9-e50e24dcca9e   (client → stick, WRITE, encrypted)
// TX char       6e400003-b5a3-f393-e0a9-e50e24dcca9e   (stick → client, NOTIFY, encrypted)
// Usage char    6e400004-b5a3-f393-e0a9-e50e24dcca9e   (client → stick, WRITE, plaintext)
//
// The usage companion writes {"session_pct":N,"weekly_pct":M}\n to the usage char
// without bonding. The desktop Hardware Buddy uses the encrypted RX/TX pair.
// Up to 2 simultaneous centrals are supported; advertising restarts on each connect.
//
// Writes from the client are line-buffered and dispatched through the
// same _applyJson path that USB/BT-Classic use. Replies (acks, status
// snapshots) are written via bleWrite() and chunked to the negotiated MTU.

void bleInit(const char* deviceName);
bool bleConnected();
int  bleConnCount();   // number of currently connected centrals (0, 1, or 2)
// True once LE Secure Connections bonding has completed for the current
// link. The NUS characteristics are encrypted-only, so in practice this
// is always true by the time any data flows; exposed so the status ack
// can report it to the desktop.
bool bleSecure();
// Non-zero while a 6-digit pairing passkey should be on screen. main.cpp
// renders it; cleared automatically on auth complete or disconnect.
uint32_t blePasskey();
// Erase all stored bonds (LTKs) from NVS. Called from the "unpair" cmd
// and from factory reset.
void bleClearBonds();
size_t bleAvailable();
int bleRead();
size_t bleWrite(const uint8_t* data, size_t len);
