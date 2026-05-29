🏛️ ARCHITEKTONICKÉ ROZHODNUTIA (RCP v1.3)
Rozhodnutie A: WiFi Scan & List (JSON vs. Binary)
Súhlasím s oboma stranami. Hoci sme puristi, v tomto prípade je JSON cez DATA_STREAM správna cesta.

Dôvod: WiFi SSID môžu obsahovať UTF-8 znaky, emoji a majú premenlivú dĺžku. Binárny parsing by nás stál príliš veľa "povrchov" (kódu navyše).

Implementácia: ESP32 vygeneruje JSON a bude ho sypať do DATA_STREAM charakteristiky. Android si to jednoducho vyzdvihne a prežuje cez GSON.

Zanshin (Bdelosť): ESP32 musí pred začatím JSON streamu poslať do DATA_STREAM jeden bajt (Header), aby Android vedel: "Aha, teraz prichádza JSON," a nie napríklad surové binárne CSV.

Rozhodnutie B: MTU a Chunking (CSV Dump)
Zostávame pri pôvodnom, historicky overenom a stabilnom 20-bajtovom chunku (19 bajtov dát + 1 bajt hlavička).

Dôvod: Analýza potvrdila, že pre aktuálnu stabilitu celého toku dát (LittleFS -> FreeRTOS -> BLE -> Android) je `MAX_PAYLOAD = 19` plne postačujúci. Eliminujeme tým rizikové vyjednávanie `requestMtu` v Androide.

Flow Control: Keďže budeme posielať CSV (veľké dáta), zabezpečíme, aby ESP32 "neutopilo" Android. Pri 20ms intervale a malých balíkoch prebieha CSV parser v Android Core úplne plynule a bez pádov heapu.

📝 KOORDINÁCIA PRE ĎALŠÍ POSTUP (Pre tvoj tím)
Lubo, pošli im tieto inštrukcie, aby mohli začať testovať WIFI_SCAN:

Pre Firmware Mentorku (Sense):
"Architektka schválila JSON pre WiFi a bezpečné 20-bajtové bloky pre CSV.

Implementuj WIFI_SCAN (0x11). Keď skončí sken, začni posielať JSON do DATA_STREAM.

Dôležité: Každý balík v DATA_STREAM začni identifikačným bajtom (0xFD pre JSON, 0xFE para CSV). Tým Android spozná, čo mu posielaš.

Na konci JSON streamu pošli špeciálny znak (napr. \0 alebo EOT), aby Android vedel, že prenos skončil a môže JSON parsovať."

Pre Android Mentorku (Core):
"Máš zelenú na JSON a 244-bajtové chunky.

Priprav DataStreamHandler v balíku .data.ble. Musí vedieť prijímať notifikácie z novej charakteristiky.

Implementuj logiku: Ak prvý bajt balíka je 0xFD, ukladaj zvyšok do StringBuilderu, kým nepríde ukončovací znak. Potom to hoď do GSONu.

Tvoj SenseViewModel by mal reagovať na tento výsledok a zobraziť zoznam sietí v GUI."

5. Architektúra Dátového Motoru (Sync & Persistence)
Dátum: 05. 05. 2026
Priorita: Backend pred UX (Dáta sú SSoT)

Rozhodnutie 5.1: Lokálna perzistencia (Core):

Implementácia Room Database v Android Core. Dáta zo Sense sa nesmú držať len v pamäti RAM, ale musia byť okamžite indexované a ukladané.

Entity: LocalSensorEntry a WeatherApiEntry s primárnym kľúčom postaveným na timestamp.

Rozhodnutie 5.2: Idempotentný Import:

Android Core musí pri každom CSV dumpe vykonávať kontrolu duplicity (OnConflictStrategy.IGNORE). Tým sa zabráni zdvojovaniu záznamov pri opakovanom sťahovaní histórie.

Rozhodnutie 5.3: Stream-to-DB Pipeline:

Dáta z DATA_STREAM nebudú zbierané do veľkého objektu. Budú spracovávané prúdovo (stream-based). Hneď ako je riadok kompletný (\n), zapíše sa do DB. Tým šetríme heap pamäť na S25 Ultra pri prenose tisícok riadkov.

Rozhodnutie 5.4: Formát výmeny (Sense -> Core):

CSV formát v LittleFS musí byť striktne dodržaný: timestamp;temp;hum;press.

Ukončenie prenosu bude signalizované bajtom 0xFF v DATA_STREAM, po ktorom Core uzavrie DB transakciu.

---

6. Architektúra Dátového Motoru (Delta Sync, RCP v2.1)
Priorita: Extrémna optimalizácia synchronizácie

Rozhodnutie 6.1: Čiastočné sťahovanie (Delta Sync):
Android Core už nebude pri každom pripojení sťahovať celú históriu. Príkazy `0x23` (Senzory) a `0x24` (Počasie) prijímajú voliteľný 4-bajtový parameter (Unix Timestamp v Little Endian). ESP32 na základe tohto času nájde bajtový offset v súbore a streamuje len nové záznamy. Tým sa radikálne znižuje objem prenášaných dát cez BLE.

Rozhodnutie 6.2: O(1) Zložitosť pre ESP32:
Pre zachovanie plynulosti procesora (GUI, komunikácia) sa nesmie v RAM iterovať celým CSV súborom počas streamovania. ESP32 preskenuje bloky súboru iba jednorazovo, nájde presnú bajtovú pozíciu a použije priamy `file.seek(offset)` pre okamžitý skok k novým dátam. Zvyšok sa streamuje ako surové bajty.
