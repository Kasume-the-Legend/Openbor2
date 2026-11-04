# OpenBOR Online (Android + PC netplay)

OpenBOR 4.0 with online play: lobby, room codes, public game list, up to 4 players.

## How it works
- Everyone runs the same game file (the lobby checks they match). Only button presses travel over the network (lockstep), so everyone sees the same game.
- Join a room (or host one). Everyone taps **Ready up**. As soon as every player in the room is ready, the game starts for all of them at once, with the same random seed.
- A relay server passes messages between players. Run `relay/relay.js` on any machine both sides can reach: `node relay/relay.js 7777`.

## Building the Android APK
1. Create a new GitHub repository and upload everything in this folder.
2. Open the **Actions** tab, pick **Build Android APK**, press **Run workflow**.
3. When it finishes, download **OpenBOR-Online-APK** from the run. Install it on your phone.

## Setting the relay address (Android)
Create a text file `netplay.ini` in `Android/data/org.openbor.engine/files/` on the phone:

    server=your.relay.host:7777
    name=YourName

## In the lobby
Tap an option. "Play offline" starts the game as normal. Android Back = go back.
