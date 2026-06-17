# UCS Presence App Reverse Engineering

This repo contains a script that was created by reverse engineering the UCS University mobile app, to fetch their classes data APIs, and respond to the attendence registration.

It will try to find a class for today, if it finds it, it will check for the open attendence registration in the app every 30s, and if it's open it will respond and exit.

## TODO:

- Multiple users?
- Better logging (aws cloudwatch?)
- Error capturing (sentry?)
- Queue operated (SQS?)
- Bug when multiple classes at the same day?


## Deploy Updates

### 12/08/2024

Hardware is all set up and configured, the brains of the operation is an Orange Pi PC, which will be connected via wired ethernet to the university network. I also added a physical serial interface, which can be accessed using an USB to Serial adapter, to open the linux shell, as the university network is very complex and I doubt I'll be able to get a SSH connection to the device reliably.

A cron was setup using crontab on the OS, which runs the script everyday at 19:40

![IMG_9509](https://github.com/user-attachments/assets/88b41141-65c7-4359-b28f-64b04ae426de)

### 14/08/2024

Attempted the first project deploy, and was defeated by the fact that the univeristy network is somehow whitelisted

Next attempt will be using eduroam wireless access points, which I have access to, although it might be somewhat less unreliable, the advantage is I can hide it anywhere I can get access to a power outlet.

Reference: [cat.eduroam.org](https://cat.eduroam.org/)

### ??/??/2024

I made it work with eduroam using the install scripts they provide
It responded my attendence
Then got stolen lol
I will try it with an ESP32 at some point in the future to make it smaller and cheaper

## ESP32 port

`esp32/find-and-answer/find-and-answer.ino` is a full port of the Node script
to an ESP32. It joins **eduroam** over WPA2-Enterprise, then runs the exact same
flow: fetch token → list classes → find today's class → poll every minute →
answer the attendance registration as soon as it opens. Logs go to Serial and
(optionally) the same Discord webhook.

### Setup

1. Install the **arduino-esp32** core (Boards Manager) — core 2.x or 3.x both
   work, the WPA2-Enterprise API difference is handled at compile time.
2. Install the **ArduinoJson** (v7.x) library via the Library Manager.
3. Copy `esp32/find-and-answer/secrets.h.example` → `secrets.h` and fill it in
   (the repo already has a prefilled `secrets.h`, which is gitignored).
   - `EAP_IDENTITY` / `EAP_USERNAME` are your eduroam login, usually
     `<user>@ucs.br` (note this differs from the SOU API username).
   - `EDUROAM_CA_PEM` can stay `nullptr` for the first test — that skips
     validating the RADIUS server cert. For a hardened setup, grab the CA from
     [cat.eduroam.org](https://cat.eduroam.org/) and paste it in.
4. Open the sketch in the Arduino IDE, select your ESP32 board, and flash.
5. Open the Serial Monitor at **115200** baud to watch it run.

### Notes / things to verify on first test

- TLS to the UCS API uses `setInsecure()` (no cert pinning) to keep it
  reliable. Fine for this use case; swap in a CA + `setCACert()` if you want it
  validated.
- If eduroam at UCS needs a specific anonymous identity or a particular EAP
  method, adjust `EAP_IDENTITY` (and the CA) accordingly — PEAP/MSCHAPv2 with
  the username login is the common default the sketch assumes.
- The sketch reboots itself on connection/token/listing failures, and deep
  sleeps after 3h (same lifetime guard as the original `setTimeout`).