# ucs-presence-reverse-engineering

This repo contains a script that was created by reverse engineering the UCS University mobile app, to fetch their classes data APIs, and respond to the attendence registration.

It will try to find a class for today, if it finds it, it will check for the open attendence registration in the app every 30s, and if it's open it will respond and exit.

TODO:

- add timeout (5h?)
- add debugging for when running on site (discord webhooks? sentry?)