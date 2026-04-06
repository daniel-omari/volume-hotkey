This is a little Windows utility I wrote in C++ so I didn't have to alt-tab each time 
I wanted to lower the volume of a game or application and risk it crashing on me just so I can hear my spotify.

How it works:
On launch, lists all apps currently producing audio
You pick which app to control
Press Ctrl+Shift+M to instantly duck that app's volume to 20%
Press it again to restore the original volume

Built with: C++, Windows Core Audio API (WASAPI), Win32 RegisterHotKey

I chose C++ cause I was bored and wanted additional challenge :P

Build: g++ -o volume_toggle.exe volume_toggle.cpp -lole32 -loleaut32 -luser32