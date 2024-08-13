# UCS Presence App Reverse Engineering

This repo contains a script that was created by reverse engineering the UCS University mobile app, to fetch their classes data APIs, and respond to the attendence registration.

It will try to find a class for today, if it finds it, it will check for the open attendence registration in the app every 30s, and if it's open it will respond and exit.

## Installation Updates

### 12/08/2024

Hardware is all set up and configured, the brains of the operation is an Orange Pi PC, which will be connected via wired ethernet to the university network. I also added a physical serial interface, which can be accessed using an USB to Serial adapter, to open the linux shell, as the university network is very complex and I doubt I'll be able to get a SSH connection to the device reliably.

A cron was setup using crontab on the OS, which runs the script everyday at 19:40

![IMG_9509](https://github.com/user-attachments/assets/88b41141-65c7-4359-b28f-64b04ae426de)

TODO:

- Multiple users?
- Better logging (aws cloudwatch?)
- Error capturing (sentry?)
- Queue operated (SQS?)