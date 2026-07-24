# LightWitch × batteryLight — Entscheidungsvorlage

Grundlage: LightWitch-Systemkonzept v1.0 (24.07.2026) und der aktuelle Stand von
`main`. Die Punkte in Teil 1 sind strukturell: sie bestimmen den Zuschnitt des
Rework-Plans und sollten vor Planungsbeginn im Team entschieden werden. Teil 2
sammelt die übrigen offenen Fragen (Konzept §11 u. a.) mit einer Empfehlung als
Arbeitsannahme — sie können den Plan mit dieser Annahme passieren und später
revidiert werden.

Legende Empfehlung: ✅ klare Tendenz · ⚖️ echte Abwägung, Teamentscheid nötig.

---

## Teil 1 · Blockierende Strukturentscheidungen

### E1 · Wer besitzt die LEDs — Graph-Welt vs. Gruppen-Welt ⚖️

**Heute:** Eine Gruppe hält `LightConfig` (Pattern/Szene/Farbe), mesh-synchron;
`PatternRunner` rendert. **Konzept:** Graphen laufen lokal, der Bühnen-Stein ist
die geräteweite Instanz des Lichts, Kanäle (Intensität, Reiz, Limit, …) steuern
die Szene.

| Option | Abwägung |
|---|---|
| **(a) Graph als neuer Modus** — `GroupMode::Graph`; steht ein Licht in diesem Modus, rendert der Bühnen-Stein statt des bisherigen Pattern-Pfads. Bestehende Patterns bleiben als prozedurale Szenen aufrufbar. | Inkrementell, Flotte bleibt während der Umstellung lauffähig, Gruppen-/Mesh-Sync unangetastet. Kostet: zwei Rendering-Pfade parallel pflegen, Übergangsregeln (was passiert bei Moduswechsel). |
| (b) Big-Bang — Graphen ersetzen Gruppen/Patterns vollständig | Konzeptionell sauber („Firmware rendert nur Pixel"), aber monatelang keine lauffähige Flotte; Verlust erprobter Features (MQTT, Sync, Overrides) bis zur Parität. |
| (c) Graph nur als Aktions-Quelle — Graphen schreiben ausschließlich über `ActionExecutor` Configs | Billigster Einstieg, aber die Kanal-Bühne (§6.3) ist damit unerreichbar; das Konzept degeneriert zur besseren Automation. |

**Empfehlung: (a)**, mit erklärtem Langfristziel Richtung (b): neue Fähigkeiten
entstehen nur noch in der Graph-Welt, der Legacy-Pfad wird eingefroren.

### E2 · Schicksal der Automation-Engine (#447/#439) ✅

Die gerade gebaute `AutomationBinding`-Tabelle (Trigger → Regeln → Aktionen) ist
funktional eine entartete Graph-Stufe. Beide Systeme parallel weiterzuentwickeln
schafft doppelte Wahrheit für „Event X → Wirkung Y".

**Empfehlung:** Automations sofort **einfrieren** (keine neuen Trigger-Typen,
keine neuen Features), Graphen subsumieren sie; Migration Bestand → Graph als
späterer Schritt. `ActionExecutor` bleibt erhalten — nicht als Nutzer-Konzept,
sondern als interne Senke der Engine (er kapselt sauber Config-Mutation +
Propagation über Mesh/MQTT).

### E3 · Engine hardwarefrei + natives Test-Env ✅

Konzept §1.8 verlangt eine Engine ohne Hardwarebezug mit Desktop-Tests. Das Repo
hat **kein** `[env:native]` und null C++-Unit-Tests; fast jeder Header zieht
`Arduino.h`, Basistypen (`Color`, `LightConfig`) leben in `Config.h`.

**Empfehlung:** Engine als eigenes Modul mit **eigenen minimalen Typen** (kein
Include aus `src/config`), Adapter übersetzen an der Grenze; `[env:native]` +
Unit-Tests ab dem ersten Engine-Commit. Das ist weniger eine Option als eine
Disziplin-Zusage — ohne sie ist §3 (Queue, Scheduler, Topo-Sort, Gelenke)
praktisch nicht verlässlich entwickelbar. Zu entscheiden ist nur, ob das Team
die CI-Pflicht (native Tests als PR-Gate) mitträgt.

### E4 · Rollen/Geräteprofil vs. bestehende Hardware-Config ✅

Konzept §4.2 will `/profil.json` (Rolle → Pin/Segment + Eichung). Das Gerät hat
aber schon eine Hardware-Config (`LightHardwareConfig`, `ButtonHardwareConfig`,
`SoundHardwareConfig` in `DeviceConfig`, NVS/LittleFS-persistiert, Web-UI-Pflege,
Config-Push übers Mesh).

**Empfehlung:** **Keine zweite Quelle der Wahrheit.** Rollen werden Namen auf den
bestehenden Hardware-Einträgen (z. B. `role: "buehne:ring"` am Light-Slot), die
Eichung ein Zusatzblock daran. „Profil" ist dann eine Sicht auf `DeviceConfig`,
kein eigenes File. Das Rollen-Matching („Graph läuft auf jedem Gerät, dessen
Profil passt") arbeitet gegen diese Einträge.

### E5 · Kanonisches Graph-Schema: Sprache & v1-Umfang ⚖️

Das Konzept skizziert deutsche JSON-Keys (`steine`, `kanten`, `braucht`).
CLAUDE.md legt Englisch für GitHub-Inhalte fest, der gesamte Code ist englisch;
Contributor-Reibung wäre real. Das Konzept trennt in §5.5 bereits Anzeige-
von Speichersprache („Feuer-Leute lesen Hitze, gespeichert wird Reiz").

**Empfehlung:** dieselbe Logik auf die Keys anwenden — **englische kanonische
Keys** (`nodes`, `edges`, `requires`), deutsche Begriffe ausschließlich als
Editor-Labels. Alternative (deutsche Keys als Produkt-Identität) ist legitim,
dann aber bewusst und dauerhaft — Umbenennen nach Release kostet eine
Schema-Migration. Zum v1-Umfang gehört außerdem: Kantenliste wie skizziert,
`v`-Feld, Rollen-Deklaration; `col/row` als reine Editor-Metadaten.

### E6 · Master-Rolle: feste Konfiguration (Konzept) vs. vorhandene Election ✅

Konzept §11.2 tendiert zu „v1 fest konfiguriert". Das Repo **hat** aber bereits
eine dynamische WiFi-Election (`WifiElection`): ein Peer hält die
WLAN-Verbindung, inkl. Retry-/GaveUp-Handling und mesh-weiter Policy.

**Empfehlung:** Konzeptpunkt zugunsten des Bestands auflösen — **Master =
gewählter WiFi-Halter**. Geräteliste, Log-Senke und Graph-Verteilung docken an
diese Rolle an. Damit ist offene Frage §11.2 ohne Neubau beantwortet; ein
explizites Wahlverfahren mit Ausfall-Übernahme bleibt v2.

### E7 · Mesh-Facade: kapseln + gezielt erweitern ✅

Konzept §7 verlangt `senden(…, ziel, bei-Änderung, Ratenlimit)` und
`empfangen(…) → {payload, absender, nähe}`. Vorhanden: Broadcast mit
targetMac-Filter, Chunk-Transfer (Scene/Playlist/Config als Kopiervorlage),
RSSI pro Peer im `PeerRegistry`, `GenericEventMsg` als Kommando-Primitiv.
Fehlend: Ratenbegrenzung, „bei Änderung", Teilnehmer-Nr-Adressierung,
Antwort-an-Absender, Sequenznummern (§12.4 — steht im Widerspruch zur heutigen
bewussten „kein ACK/keine Seq"-Linie bei `GenericEvent`).

**Empfehlung:** ESP-NOW-Schicht behalten, dünne Facade nach §7 darüber;
Nähe aus dem `PeerRegistry` in den Empfangs-Callback durchreichen.
Sequenznummern der `GenericEvent`-Klasse hinzufügen (Wire-Policy erlaubt
Breaking Changes, da ohnehin nur gleiche Firmware kompatibel ist) — billig und
liefert die Verlustrate aus §12 gratis. Teilnehmer-Nr erst mit dem
Gruppe-Stein (Meilenstein 9).

---

## Teil 2 · Nicht-blockierend — Empfehlung als Arbeitsannahme

| # | Frage (Konzept-Ref) | Empfehlung | Begründung / Anmerkung |
|---|---|---|---|
| N1 | Zeitsync für Buzzer-Fairness (§11.3) | v1 „erste Nachricht beim Master gewinnt" | Vorhandener `TimeSync` liefert nur Sekunden; ms-Mesh-Zeit ist Neubau → v2, erst nach Messung des tatsächlichen Druckabstands (§12.5). |
| N2 | Laufzeitzustand über Neustart (§11.4) | flüchtig; „merken"-Flag später | Deckt sich mit Flash-Verschleiß-Argument (§12.4); Bestand schreibt Config ohnehin nur auf explizite Änderungen. |
| N3 | Farb-Interna / RGBW (§11.5) | HSV innerhalb Engine + Bühne, Wandlung nach RGB an der `LedDriver`-Grenze; RGBW vertagen | Bestand ist durchgehend RGB; Wandlung an einer Stelle hält den Umbau klein. Es gibt heute keine unterstützte RGBW-Hardware. |
| N4 | LED-Map-Erstellung (§11.6) | Presets je Bauform (Strip/Ring/Matrix → generierte Richtungsvektoren); Kamera-Wizard später | `MatrixLayout` liefert die Matrix-Geometrie schon; Map-**Format** gehört trotzdem ins Schema v1, sonst Migration. |
| N5 | Kugel-Maleditor (§11.7) | nach Meilenstein 5, wie im Konzept | Bestehender Szenen-Editor (flache w×h-Frames) bleibt bis dahin das Mal-Werkzeug. |
| N6 | Klang-Pipeline (§11.8) | v1 = Töne + kurze Samples auf Bestand aufsetzen | WAV-von-SD, Playlists und synchronisierter Start existieren (`PlayAudioMsg`); neu ist nur ein kleiner Ton-Generator. Mischen/Formate vertagen. |
| N7 | Web-Schreibschutz (§11.9) | PIN für schreibende Endpunkte, spätestens vor Graph-Push-Features | Geräte-API ist heute offen; mit Graph-Verteilung wächst die Angriffsfläche deutlich (beliebige Abläufe pushbar). |
| N8 | Graph-Migration (§11.10) | bestehender Schema-Migrations-Konvention des Repos folgen | Ist dokumentiert (Commit d05e7ca); kein neues Verfahren erfinden. |
| N9 | Dock-Feinheiten: Elbow-Toleranz, Zoom (§11.11) | nach ersten Editor-Tests | Editor v1 ist ohnehin REST + JSON-Textfeld (§5.6). |
| N10 | OTA übers Mesh (§11.12) | **streichen** | GitHub-OTA + Mesh-Nudges (`CheckUpdate`/`TriggerUpdate`) existieren und funktionieren; Mesh-Chunk-Verteilung bleibt Graphen/Szenen vorbehalten. |
| N11 | Abnahme-Zielwerte (§11.13) | erst messen, dann festzurren — wie im Konzept | Sonden-Infrastruktur (§12.1) früh einbauen, Ziele nach Meilenstein 2 setzen. |
| N12 | Mikrofon / Lagesensor | Hardware-Auswahl bis Meilenstein 6 treffen | Keinerlei Treiber-Grundlage im Repo; einzige Quelle mit Bestand ist `BatteryMonitor`. Betrifft Meilenstein 7, nicht den Planstart. |

---

## Was der Rework-Plan als Input braucht

Entschieden sein müssen **E1–E7** — sie bestimmen Modulschnitt (E1–E4),
Dateiformat (E5) und Mesh-/Master-Anbindung (E6–E7). Für **N1–N12** reicht es,
die Empfehlung als Annahme zu übernehmen oder einzeln zu überstimmen; keiner
dieser Punkte ändert den Zuschnitt der ersten Meilensteine (§10, Stufen 1–4).
