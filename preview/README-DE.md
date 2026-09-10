# Horizon Lock VR – Vorschau 0.4.0

Die Standalone-App übernimmt dein vorhandenes iRacing-Vorschaufenster, glättet kleine Bildbewegungen und zeigt das Ergebnis in einem eigenen Fenster für die Capture Card. **Kein iRacing-Neustart, keine Verbindungseinrichtung und kein OBS auf dem Gaming-PC sind dafür nötig.** Das Brillenbild bleibt unverändert.

## Direkt starten

1. **Gaming-PC\Start-Preview.cmd** öffnen.
2. **Vorschaufenster aufnehmen (ohne Neustart)** verwenden. Das laufende iRacing-Simulatorfenster wird nach Möglichkeit automatisch ausgewählt. Sonst **Aktualisieren** klicken und das gewünschte Vorschaufenster auswählen.
3. Das Fenster **Clean Preview** auf den Bildschirm für die Capture Card ziehen. **F11** oder Doppelklick schaltet Vollbild; Escape verlässt es.
4. Das ursprüngliche iRacing-Vorschaufenster geöffnet lassen. Es darf verdeckt sein, aber nicht minimiert werden. Wird es geschlossen, das neue Fenster nach dem nächsten Spielstart erneut auswählen.

Die normale Fensteraufnahme verwendet **Bildanalyse** und ist vom VR-Treiber unabhängig: iRacing kann über OpenVR, Pimax OpenXR oder SteamVR mit SBoys3 laufen. Sie benutzt keine Headset-Pose. Die Analyse verfolgt bevorzugt Cockpitstruktur in der unteren Bildhälfte; große Szenenwechsel und fehlende Bilddetails können die Erkennung einschränken. Es gibt keine Garantie, dass jede Bewegung oder jeder bisherige Sprung dadurch verschwindet.

## Einstellen und vergleichen

- Startwerte: 40 % Glättung, 10 % Crop-Reserve, 60 FPS.
- **Ränder spiegeln** füllt Bereiche außerhalb des Bildes mit gespiegeltem Bildinhalt. Es rekonstruiert keine fehlende Geometrie.
- **Reset Vorschau** setzt die Glättung dieses Fensters zurück, ohne die Brille neu zu zentrieren.
- **Stabilisierung aus** zeigt denselben aufgenommenen Frame mit gleichem Ausschnitt ohne Korrektur. Die Analyse läuft für den Vergleich weiter.
- **Pause / GPU-Vergleich** stoppt Fensteraufnahme und laufendes Rendering. Das Standbild bleibt sichtbar. Nutze den Schalter bei gleicher Spielsituation und beobachte die VR-Frametimes. Minimieren oder Schließen des Clean-Preview-Fensters pausiert ebenfalls.
- Die Ausgabe ist 16:9 mit maximal 1920 × 1080 interner Auflösung; größere Fenster werden hochskaliert. 30 FPS reduziert Verarbeitung, 90 FPS erhöht sie.
- Die Fensteraufnahme ist für SDR ausgelegt. HDR-Farben sind nicht gesondert tonemapped.

Auf dem Stream-PC das bereits stabilisierte Vorschaufenster von der Capture Card verwenden. Den bisherigen OBS-Stabilisierungsfilter für diese Quelle deaktivieren, damit das Bild nicht zweimal bearbeitet wird. Die App ändert deine OBS-Konfiguration nicht.

## GPU-Aufwand

Die App übernimmt fertige Bilder, rendert die Spielszene nicht erneut und encodiert kein Video. Fensteraufnahme, Bildkopien, ein Grafikfilter und die Darstellung kosten trotzdem GPU-Zeit und Speicherbandbreite. Die Bildanalyse liest nur eine kleine, maximal 480 × 270 große Version zurück und läuft auf der CPU. Bild und Analyse bleiben dabei zusammen; die Ausgabe wartet üblicherweise einen zusätzlichen Frame auf das passende Analyseergebnis. Bei Rückstau werden neue Frames übersprungen, statt unbegrenzt zu puffern.

Eine konkrete Ersparnis gegenüber OBS ist nicht gemessen. Die tatsächlichen VR-Frametimes mit **Pause / GPU-Vergleich** vergleichen. Hohe Auflösung des ursprünglichen Fensters kann den Kopieraufwand auch bei 1080p-Ausgabe erhöhen.

## Optionale OpenXR-Pose-Aufnahme

**OpenXR-Pose direkt** ist ein zusätzlicher Aufnahmeweg für Windows x64 / D3D11. Er übernimmt VR-Bilder mit der zugehörigen abgegebenen Pose und korrigiert Rotation. Stereo und vier Ansichten mit fovealem Ausschnitt werden unterstützt. Der Weg ist für Pimax OpenXR sowie SteamVR OpenXR mit SBoys3 vorgesehen; die reale Kombination muss auf dem jeweiligen Setup getestet werden.

Nur für diese Option einmal **Verbindung einrichten** klicken und anschließend iRacing im **OpenXR-Modus** neu starten. Die App registriert ausschließlich ihre eigene OpenXR-Layer für deinen Windows-Benutzer. Sie ändert weder die aktive Runtime noch andere Layer. Der iRacing-Startmodus OpenVR spricht diese optionale Layer nicht an.

Für den direkten Pose-Weg ist kein Tracking-Sender und kein Netzwerk-/Capture-Delay-Abgleich nötig. Positionsbedingte Parallaxe, Bewegungsunschärfe und die spätere Runtime-Reprojektion bleiben außerhalb dieser Rotationskorrektur. **Verbindung entfernen** löscht nur die Registrierung dieser Paketkopie; anschließend das Spiel neu starten. Vor einem Wechsel auf eine andere Preview-Version die bisherige Verbindung entfernen. `DISABLE_HORIZONLOCK_PREVIEW=1` deaktiviert die Layer für einen Spielprozess.

## Dateien und Tests

Das komplette Preview-Verzeichnis zusammen behalten. Einstellungen liegen in `preview.ini` neben der EXE. `horizonlock-preview.exe --demo` zeigt ein synthetisches Prüfbild. Native automatisierte Tests prüfen reale Windows-Fensteraufnahme, GPU-Verarbeitung, Größenänderungen, Lebensdauer und OpenXR-Aufrufe mit einer Test-Runtime. Eine Sichtprüfung beim echten Fahren und eine Messung des GPU-Aufwands stehen separat an.
