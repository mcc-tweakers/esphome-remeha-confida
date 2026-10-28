# Remeha Confida uitlezen met een ESP32 (ESPHome)

Lees een Remeha Confida (EHC-regelaar) live uit in **Home Assistant** via de
**onboard Bluetooth** van het toestel — **geen GTW-08-gateway nodig**, geen cloud. 
Een ESP32 met ESPHome praat de
onboard BLE-taal van het toestel: het koppelt via Bluetooth, en zet temperaturen, druk, debiet, COP,
vermogens, tellers en storingen door naar Home Assistant. Een aantal parameters
(stooklijn, drukwaarschuwing) is ook in te stellen.

Gebruik op eigen risico.

## In de huidige ESPHome-Yaml

- **~45 sensoren** — aanvoer/retour, buitentemperatuur, waterdruk, debiet, COP,
  elektrisch en thermisch vermogen, compressor- en ventilatortoerental, modulatie,
  pompstanden en de status van de buitenunit.
- **Tellers** (`total_increasing`) — energieverbruik en -levering, productie-uren,
  starts en ontdooicycli.
- **8 instelbare parameters** (`number`) — o.a. stooklijn (voetpunt, helling,
  max. aanvoer, ruimte-invloed) en de lagedrukwaarschuwing.
- **Storingsstatus** — of de warmtepomp in storing staat (HM007). De volledige
  fouthistorie (`H{categorie}.{nummer}`) zit wél in het protocol (zie
  [`docs/readme.md`](docs/readme.md)), maar deze YAML leest alleen de status uit.
- **Diagnose** — laatste schrijfresultaat, herverbindreden en -teller.
- Een **schakelaar** om de uitlezing tijdelijk los te laten en een **herstartknop**.

### Zelf een sensor of parameter toevoegen

Een waarde toevoegen is **alleen YAML-werk** — je hoeft de component (C++) niet aan
te raken. De lijst hierboven is maar een selectie; het toestel kent duizenden
objecten.

**Stap 1 — zoek het object op.** Gebruik het codeboek
`data/Confida_parametercodeboek.xlsx`, tab **"Confida"** = de objecten die dít
toestel gebruikt (met Nederlandse namen). Zoek op de code (bv. `HM001`)
of op een trefwoord in de naam. De kolommen vertalen direct naar YAML-velden:

| kolom in het codeboek | YAML-veld |
|---|---|
| CANopen index | `index` (zet er `0x` voor) |
| Sub | `subindex` (arrays: waarde op `1`) |
| Datatype | `value_type` (in kleine letters) |
| Gain | `gain` |
| Eenheid | `unit_of_measurement` |
| Soort | `Parameter` = instelbaar (`number`); `Signal`/`Counter` = alleen lezen (`sensor`) |
| Uitlezen | `read` (`bulk` of `single`) |
| Toegestane waarden (NL) | enkel bij `value_type: enum` |

De kolom **"Soort"** bepaalt of een object een instelling of een meetwaarde is, en
daarmee of het onder `sensor:` of `number:` hoort:

- **`Parameter`** — een echte instelling. Als je 'm wilt kunnen wijzigen: zet 'm onder
  `number:`. Wil je 'm alleen tonen: zet 'm als `sensor:` (dan schrijf je niets naar
  het toestel). Een `number` schrijft een echte waarde naar het toestel, dus wees
  voorzichtig.
- **`Signal`** — een live meetwaarde (temperatuur, druk, toerental…). Altijd `sensor:`,
  meestal met `state_class: measurement`.
- **`Counter`** — een oplopende teller (energie, uren, starts). `sensor:` met
  `state_class: total_increasing`.

**Stap 2 — zet het blok in de YAML**, onder `sensor:` (alleen lezen) of `number:`
(instelbaar).

Sensor toevoegen (onder `sensor:`):

```yaml
  - platform: remeha_confida
    name: "Aanvoertemperatuur van warmtepomp (HM001)"
    index: 0x4301          # kolom "CANopen index", met 0x ervoor
    value_type: i16        # kolom "Datatype" in kleine letters (u8/i8/u16/i16/u32/i32/enum/bool)
    gain: 0.01             # kolom "Gain": ruwe waarde × gain = de echte waarde (leeg in het codeboek = 1)
    unit_of_measurement: "°C"   # kolom "Eenheid"
    device_class: temperature   # HA-weergaveklasse (temperature/pressure/power/energy/…), niet van het toestel
    state_class: measurement    # measurement = momentane waarde; total_increasing = teller
    accuracy_decimals: 1        # aantal decimalen in HA (stem af op de gain)
    icon: mdi:thermometer       # vrij te kiezen mdi-icoon
```

Velden die je meestal weglaat: `subindex:` (standaard 0; bij array-objecten zet je
`subindex: 1` voor de actieve zone/teller), `node:` (standaard 1; andere module →
zie de node-map in `docs/readme.md`) en `read:` (standaard `bulk`; zet `single`
voor losse scalars die níet in de bulk-GetList zitten — zie de kolom "Uitlezen").

Instelbare parameter toevoegen (onder `number:`):

```yaml
  - platform: remeha_confida
    name: "Waarschuwing dat druk laag is (AP058)"
    index: 0x302B          # kolom "CANopen index", met 0x ervoor
    value_type: u8         # kolom "Datatype" in kleine letters
    gain: 0.1              # kolom "Gain"
    unit_of_measurement: "bar"  # kolom "Eenheid"
    device_class: pressure      # HA-weergaveklasse
    read: single           # kolom "Uitlezen": losse schrijfbare scalars vaak 'single'; zone-/stooklijnobjecten (0x34xx) 'bulk'
    min_value: 0.3         # alleen HA-UI-grens (NIET op de draad afgedwongen) — kies realistisch of de technische type-range
    max_value: 3.0         # idem
    step: 0.1              # stapgrootte in de HA-interface
    mode: box              # HA-invoerstijl: 'box' (invoerveld) of 'slider'
    entity_category: config     # toont 'm bij de configuratie-entiteiten in HA
    icon: mdi:gauge-low
```

Een `number` wordt óók meegepolld, zodat de entiteit de werkelijke, teruggelezen
toestelwaarde toont — niet alleen de laatst geschreven waarde. Voor **enum**-waarden
zet je `value_type: enum`; de toegestane keuzes staan in de kolom "Toegestane
waarden (NL)" of in het tabblad **"Enums"** van het codeboek.

> Een `number` wijzigt een **echte toestelinstelling**. Twijfel je, maak er dan een
> alleen-lezen `sensor` van (zelfde velden, onder `sensor:`).

## Benodigdheden

- Een **ESP32** (standaard `esp32dev`; pas het board aan in de YAML als je iets
  anders hebt). BLE moet aanstaan, dus geen ESP8266.
- **ESPHome** (los, of via de Home Assistant-add-on).
- Het **6-cijferige nummer van de BLE-sticker** op je Confida (de pairing-passkey).

## Installatie

1. Zet `remeha-confida.yaml` en de map `components/` samen in je ESPHome-config.
2. Maak een `secrets.yaml` aan (zie `secrets.yaml.example`) met je wifi-gegevens en
   `remeha_bt_key` — het stickernummer. **Zet het tussen aanhalingstekens** als het
   met een nul begint, anders leest YAML het als octaal:
   ```yaml
   remeha_bt_key: "012345"
   ```
3. Compileer en flash. Standaard hoef je **geen MAC in te vullen** — zie hieronder.
4. Koppelen: zorg dat er op dat moment **geen** andere BLE-verbinding met het toestel
   actief is (bv. een telefoon-app — zie "Eén verbinding tegelijk"). Bij de eerste keer koppelen voert de ESP de passkey
   zelf aan; je hoeft niets in te typen.

## Automatisch verbinden (of een vast MAC)

Laat `ble_mac` onder `substitutions` op `00:00:00:00:00:00` staan: de ESP zoekt dan
zelf naar een toestel waarvan de naam begint met `EHC-` en verbindt met het eerste
dat hij vindt. Elk gezien BLE-apparaat verschijnt éénmalig in de ESPHome-log:

```
[remeha_confida] BLE gezien: 00:80:E1:AA:BB:CC  naam='EHC-16'  rssi=-62
[remeha_confida] Confida gevonden: EHC-16 (00:80:E1:AA:BB:CC) - verbinden
```

Heb je **meerdere** toestellen in bereik, lees dan het juiste MAC uit die log af en
zet het bij `ble_mac` in de YAML; dan verbindt de ESP altijd met dat ene toestel en
slaat het scannen over. Het MAC vind je ook met een
[nRF Connect](https://play.google.com/store/apps/details?id=no.nordicsemi.android.mcp)-scan.
De naam-prefix is desgewenst aan te passen met `device_name_prefix` onder
`remeha_confida`.

## De schakelaar "Remeha uitlezing"

Er mag maar **één** BLE-centrale tegelijk verbonden zijn. Zet de schakelaar op **UIT**
om de ESP de verbinding te laten loslaten en te laten stoppen met herverbinden, zodat
een telefoon of installateur kan verbinden. **AAN** = weer uitlezen. Na een
herstart staat hij standaard aan.

## Parameters schrijven — let op

De `number`-entiteiten wijzigen **echte toestelinstellingen**. Gebruik ze alleen als
je weet wat je doet. De `min`/`max`/`step` in de YAML zijn enkel grenzen voor de
Home Assistant-interface; het toestel weigert een ongeldige waarde zelf (je ziet dan
een melding in de sensor "laatste schrijfresultaat"). Na een wijziging leest de ESP
de waarde terug, zodat de entiteit de werkelijke toestelwaarde toont.

## Problemen oplossen

- **Komt niet voorbij het koppelen / "auth fail":** bijna altijd een verkeerde
  passkey. Controleer `remeha_bt_key` (het stickernummer) en de aanhalingstekens.
  In de opstartlog zie je `passkey: ****xx` — de laatste twee cijfers ter controle.
- **"Repeated Attempts" / blijft falen:** na een verkeerde pincode is de gateway
  **60 s niet beschikbaar**. De component wacht daarom oplopend (90 s → 3 → 5 → 10 min)
  voor een nieuwe poging; even geduld, niet handmatig blijven herstarten.
- **Een telefoon kan niet verbinden:** de ESP is verbonden. Zet "Remeha uitlezing" uit.
- **Een sensor blijft leeg terwijl de index klopt:** veel objecten zijn arrays — de
  waarde staat dan op `subindex: 1` (of 2), niet op 0.
- **Maximaal 3 gekoppelde apparaten:** koppel je een 4e, dan vergeet de gateway het
  oudste. Koppel de ESP dus bij voorkeur als laatste.

## Hoe het werkt

De volledige technische beschrijving — pairing, de twee challenge/response-gates,
het frameformaat en de lees-/schrijfoperaties — staat in
[`docs/readme.md`](docs/readme.md). Genoeg om het protocol en de component van
binnen te begrijpen.

## Mapindeling

```
remeha-confida.yaml        de ESPHome-config: sensoren, parameters, schakelaar
components/remeha_confida/  de external component (C++/Python-driver)
docs/readme.md              technische protocolreferentie
data/                       codeboek om objecten op te zoeken
secrets.yaml.example        voorbeeld voor je eigen secrets.yaml
```
