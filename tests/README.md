# Host-Unittests

Laufen auf dem PC, nicht auf dem ESP32. Sie prüfen die Byte-Logik des
Telnet/Raw-Pfads, die in `firmware/src/net.cpp` steckt, und die Akku-Kennlinie
samt Kalibrierung aus `firmware/src/bat_curve.h`.

    g++ -std=c++17 -Wall -o t iac_filter_test.cpp && ./t

    g++ -std=c++17 -Wall -o t3 bat_curve_test.cpp && ./t3

`bat_curve_test.cpp` bindet die ausgelieferte Header-Datei direkt ein.

`telnet_iac_test.cpp` erwartet die Klasse `TelnetSession` als `extracted.inc`
daneben. Erzeugen:

    python3 - <<'PY'
    src = open("../firmware/src/net.cpp").read()
    a = src.index("static const uint8_t T_IAC")
    b = src.index("static TelnetSession tcpSession[MAX_PORTS];")
    open("extracted.inc", "w").write(src[a:b].replace("static const uint8_t T_IAC", "const uint8_t T_IAC", 1))
    PY
    g++ -std=c++17 -Wall -o t2 telnet_iac_test.cpp && ./t2

So wird wirklich der ausgelieferte Code getestet und keine Kopie, die
auseinanderlaufen kann.
