# Remeha Confida 40 — BLE-protocol (technische referentie)

Deze tekst beschrijft hoe je een Remeha Confida 40 rechtstreeks over zijn onboard
Bluetooth Low Energy uitleest en bijstelt: de koppeling (pairing), de twee
challenge/response-"gates", het frameformaat en de lees-/schrijfoperaties. Het is
bedoeld als referentie bij de ESPHome-component in deze repo — genoeg om zowel het
protocol als de code te begrijpen, niet meer.

De waarden en voorbeelden hieronder zijn van een Confida 40E. Het BLE-MAC is
uniek per toestel. De UUID's zijn naar verwachting gelijk voor de hele
Confida-serie; controleer ze eventueel eenmalig met een
[nRF Connect](https://play.google.com/store/apps/details?id=no.nordicsemi.android.mcp)-scan
— daar lees je meteen ook het MAC af.

---

## 1. Transportlaag (BLE / GATT)

Alle verkeer loopt over één GATT-service met twee characteristics:

| richting | characteristic | handle | aanzetten |
|---|---|---|---|
| ESP → toestel (write) | write | 0x0E | — |
| toestel → ESP (notify) | notify | 0x10 | CCCD 0x11 |

UUID's van dit toestel (vast voor de Confida-serie, maar controleer de jouwe):

```
service  f8fc98e4-5919-4a5c-852e-dfe04ad383c0
write    496b1b03-cebc-4d59-9c32-14ea88c266f9
notify   ab9af948-fb86-492d-820d-bdea2dcb7ecf
```

Daarnaast heeft het toestel een vast identiteitsadres (voorbeeld
`00:80:E1:AA:BB:CC`) en adverteert het zijn naam als `EHC-...`.

Belangrijke randvoorwaarde: **er mag maar één BLE-centrale tegelijk verbonden
zijn.** Zolang de ESP verbonden is, kan de telefoon-app er niet bij, en omgekeerd.

---

## 2. Pairing

De koppeling is standaard **BLE LE Secure Connections, Passkey Entry**. De passkey
is het **6-cijferige getal op de BLE-sticker** van het toestel. De ESP geeft
IO-capability "keyboard only" op en levert dat getal zelf aan. Ná de bonding is de
applicatielaag **plaintext** — er zit geen extra versleuteling op de berichten.

### 2.1 Koppelgedrag van de interne gateway

De BLE-gateway in het toestel (node 5, "GTW-Blue") stelt twee harde regels die je
in de code terugziet:

- **Verkeerde pincode → de gateway is 60 s niet beschikbaar.** Elke koppelpoging
  in dat venster faalt met SMP-reden "Repeated Attempts". Wie binnen die 60 s blijft
  herproberen, houdt zichzelf permanent buiten. De component wacht daarom na een
  mislukte koppeling oplopend (90 s → 3 → 5 → 10 min) in plaats van direct opnieuw.
- **Maximaal 3 gekoppelde apparaten.** Koppelt een 4e, dan vergeet de gateway het
  oudste. Is dat de ESP, dan werkt zijn opgeslagen sleutel niet meer; de component
  wist dan éénmalig zijn eigen bond en koppelt vers na de wachttijd. Praktisch:
  koppel de ESP als laatste en koppel daarna niet nog drie andere toestellen.

### 2.2 Foutcodes lezen

ESP-IDF meldt een koppelfout als `auth fail reason` = **77 (0x4D) + de SMP-reden**:

| waarde | SMP-reden | meestal |
|---|---|---|
| 0x51 | Confirm Value Failed | verkeerde passkey |
| 0x53 | Encryption Key Size | key-size-mismatch |
| 0x56 | Repeated Attempts | de 60 s-blokkade (zie 2.1) |

Een klassieke keten: verkeerde `passkey` → 0x51 → snelle herhaalpogingen → daarna
aanhoudend 0x56. Controleer de passkey in de opstartlog (`passkey: ****xx`, de
laatste 2 cijfers) en zet hem **tussen aanhalingstekens** in `secrets.yaml` — een
getal met voorloopnul leest YAML anders als octaal.

---

## 3. De twee "gates" (challenge/response)

Pairing alleen is niet genoeg. Het toestel wil na het verbinden **twee keer** een
correct berekend antwoord zien voordat het data vrijgeeft. Niets in de GATT-tabel
verraadt dat; het is puur een gedragsregel van het toestel. Beide gates gebruiken
hetzelfde TEA-achtige rekenrecept.

### 3.1 Gate-service (aparte characteristics)

Naast de datakanalen heeft het toestel een aparte "gate"-service met vier
characteristics (handles 0x1B / 0x1D / 0x20 / 0x22):

1. Schrijf **0 bytes** naar **0x1B**.
2. Het toestel stuurt een **8-byte challenge** op **0x1D**: `serial` (4 bytes LE) +
   `chal` (4 bytes LE).
3. Bereken het antwoord met het recept in 3.3 en schrijf `level` + de 8 antwoordbytes
   naar **0x20**.
4. Het toestel bevestigt op **0x22**.

### 3.2 CANIP-handshake (over de datakanalen)

Daarna volgt een tweede gate, maar nu over de gewone datakanalen (0x0E/0x10): een
"session-start"-frame, opnieuw een TEA-challenge/response, en een afsluitend
follow-up-frame. Pas als die rond is, staat de sessie in de leesstand (RUN) en
beantwoordt het toestel reads en writes.

### 3.3 Het rekenrecept

Een TEA-variant over 32 rondes. `level` is het gevraagde toegangsniveau (4 in de
praktijk); `serial` en `chal` komen uit de challenge:

```
delta = 0x9E3779B9
k0    = 0x15EFA43F
sum   = 0
v0 = v1 = level

herhaal 32x:
  v0  += (k0   + (v1 << 4)) ^ (serial + (v1 >> 5)) ^ (v1 + sum)
  v1  += (chal + (v0 << 4)) ^ (sum    + v0)        ^ (level + (v0 >> 5))
  sum += delta

antwoord = (v0, v1)   # elk 32-bit, little-endian in het antwoordframe
```

---

## 4. Frameformaat

Let op het verschil tussen wat er **op de draad** staat (fragmenten met een
seq-byte) en het **samengestelde bericht** (waarop de CRC en de logica werken).

### 4.1 Fragmentatie

Elk bericht wordt in stukjes van ≤ 20 bytes als BLE-notificatie verstuurd. Elk
fragment is `[seq][payload]`: `seq` telt 0, 1, 2, … en het **laatste** fragment
heeft `seq = 0xFF`. Reassembleren = de seq-byte van elk fragment weghalen en de
payloads aan elkaar plakken tot je een fragment met `seq = 0xFF` ziet.

> Valkuil: "antwoorden beginnen niet altijd met 0xFF." Dat klopt — die 0xFF is de
> seq-byte van het (enige, dus laatste) fragment, niet het begin van het bericht.
> Reassembleer dus op fragmentniveau, niet op een vaste startbyte.

### 4.2 Het samengestelde bericht

```
offset  bytes  betekenis
[0:5]   5      marker     01 02 00 00 00   <- elk bericht begint hiermee
[5]     1      operation  zie 4.4
[6]     1      reserved   0x00
[7:]    ...    operatie-specifieke payload
[-2:]   2      crc        CRC-16/MODBUS, BIG-endian
```

Er staat **geen** extra 0xFF vóór de marker. De 0xFF die je bij korte reads/writes
aan het begin ziet, is de seq-byte van het fragment (4.1). Voeg je er zelf nog een
0xFF aan toe, dan negeert het toestel het bericht zonder antwoord of foutmelding.

### 4.3 CRC

**CRC-16/MODBUS**: polynoom 0x8005 (gereflecteerd 0xA001), init 0xFFFF,
refin/refout = true, xorout = 0. Opgeslagen **big-endian**, berekend over het hele
samengestelde bericht behalve de twee CRC-bytes zelf (dus vanaf de marker).

### 4.4 Operatiebytes (offset [5])

| op | betekenis |
|----|-----------|
| 0x00 | read request (losse waarde) |
| 0x01 | read response |
| 0x02 | read error |
| 0x04 | write request |
| 0x05 | write response |
| 0x06 | write error |
| 0x08 | GetList (bulk) request |
| 0x09 | GetList (bulk) response |

---

## 5. Operaties

Een object wordt altijd geadresseerd met **node** (1 byte), **index** (2 bytes
**big-endian**) en **subindex** (1 byte). De waardebytes zijn **little-endian**.

### 5.1 Losse read (0x00 → 0x01)

Request: `0102000000 | 00 | 00 | <node> <idxHi> <idxLo> <sub> | <crc>`
Response: idem met op = 0x01, gevolgd door de waardebytes (little-endian).

Voorbeeld — toestelnaam lezen (index 0x300F, sub 0, node 1), op de draad met
seq-byte 0xFF ervoor:

```
FF 01 02 00 00 00  00 00  01 30 0F 00  D1 A7
│  └ marker ─────┘  │  │   │  └idx┘ └sub│  └ CRC
seq                 op res node          (BE)
→ response levert "EHC-16"
```

### 5.2 Write (0x04 → 0x05)

Request: marker + `04 00 <node> <idxHi> <idxLo> <sub>` + waarde little-endian
(`raw = waarde / gain`) + CRC. Het toestel antwoordt met een **lege** write-response
(op 0x05). Standaard wordt daarna direct teruggelezen ter controle.

Voorbeeld — AP058 (index 0x302B, sub 0) naar 0.8 bar (type U8, gain 0.1 → raw 0x08):

```
FF 01 02 00 00 00  04 00  01 30 2B 00  08  B7 94
                   │       │  └idx┘ sub │   └ CRC
                   op write node        waarde (1 byte, LE)
```

Volledige wijzigingscyclus zoals het toestel die verwacht:
**read → write (0x04) → write-response (0x05) → read (controle)**.

### 5.3 GetList / bulk (0x08 → 0x09)

De efficiënte leesmethode: één request somt alle gewenste objecten op, het toestel
stuurt in één (gefragmenteerd) frame alle waarden terug — **zonder** per-record CRC.

Request-payload: `08 00 <count>`, daarna per object `00 <node> <idxHi> <idxLo> <sub>`.

Response-records, vanaf offset [7] in het samengestelde bericht, achter elkaar:

```
[flag:1][node:1][index:2 BE][sub:1][status:2][len:1][value:len]
```

- `flag` 0x01 = ruwe waarde in `len` bytes; 0x02 = "typed" (2-byte waarde + 2-byte
  type-tag).
- `status ≠ 0x0000` betekent "niet beschikbaar" → sla dat record over.

Robuust parsen: plak de hele notify-blob aan elkaar en scan dit record-patroon.
In één ronde haal je zo alle monitoring-waarden én de fouthistorie binnen.

---

## 6. Objecten decoderen

De component bevat een decodeertabel die `(index, subindex)` koppelt aan **type**
(U8/I8/U16/I16/U32/I32/Enum/Bool), **gain** (schaalfactor), **unit** en een naam.

Decoderen: lees de waardebytes als little-endian integer (met teken volgens het
type) en vermenigvuldig met `gain`. Sentinelwaarden betekenen "geen meting":
`0x8000` voor signed-16 (= −327.68 bij gain 0.01, "sensor niet gemonteerd"),
`0xFFFF` / `0xFFFFFFFF` voor unsigned.

### 6.1 Arrays en subindex — de belangrijkste regel

Veel objecten zijn arrays (max. 5 elementen). De echte waarde staat dan **niet op
subindex 0** maar op subindex ≥ 1. Twee grote families:

- **Zone-objecten** (CP 0x34xx = instellingen, CM 0x54xx = meetwaarden): de waarde
  van de actieve zone (CIRCA) staat op **subindex 1**.
- **Energietellers** (0x51xx): per functie op **subindex 1** (bv. CV), niet de som
  op subindex 0.

Vuistregel: geeft een object met een geldige index tóch NA/0, probeer dan eerst
subindex 1 (en 2) voordat je iets exotisch concludeert.

### 6.2 Toegestane waarden (enums)

Voor enum-parameters liggen de toegestane keuzes vast (waarde → omschrijving); die
staan in de decodeertabel. **Numerieke** min/max/stap horen níet bij het protocol —
het toestel geeft ze niet terug. In HA zijn `min`/`max`/`step` op een `number`
alleen UI-grenzen; ze worden niet op de draad afgedwongen. Een out-of-range write
wordt door het toestel zelf geweigerd met een foutantwoord (zie 7.1).

### 6.3 Statusvlaggen (0x502E)

De statusbits in 0x502E (sub 1..7, o.a. vorstbeveiliging, warmwater, "CH enable
input") zijn booleans met **0 = nee** en **0xFF = ja**. Decodeer ze als `bool`
(publiceert 0/1), niet als enum (die zou 255 tonen).

---

## 7. Fouten

### 7.1 Foutantwoorden (SDO-abort)

Een mislukte read antwoordt met op **0x02**, een mislukte write met op **0x06**. Na
node/index/sub volgt een 4-byte **CANopen SDO-abortcode, little-endian**. Gangbare
codes:

| code | betekenis |
|---|---|
| 0x06020000 | object bestaat niet (o.a. bij node 0xFF) |
| 0x06010002 | alleen-lezen |
| 0x06090030 | waarde buiten bereik |
| 0x06090031 | waarde te hoog |
| 0x06090032 | waarde te laag |
| 0x08000020/21/22 | kan niet opslaan / lokale besturing / verkeerde toestand |

Op de draad staat de code dus omgekeerd, bv. `30 00 09 06` = 0x06090030.

### 7.2 Fouthistorie (live leesbaar)

- **Actieve fout:** 0x1003:01 = 0xFFFF betekent *geen* actieve fout.
- **Historie:** 0x1003:subN (N ≥ 1) = foutcode, 2 bytes **big-endian** = (nummer,
  categorie) → weergave `H{categorie}.{nummer}`.
- **Tijdstempel:** 0x2002:subN = 4 bytes ms sinds middernacht (LE) + 2 bytes dagen
  sinds 1984-01-01 (LE).

Deze objecten komen mee in de GetList (op 0x09). De foutcode-weergave is
`H{categorie}.{nummer}`; welke codes bestaan ligt vast in de decodeertabel.

---

## 8. Node-map (dit toestel)

| node | naam | rol |
|------|------|-----|
| 0x01 | EHC-16 | warmtepompregelaar (varHp 0x43xx, varAp 0x50xx) |
| 0x03 | MK2.1 | bedieningsmodule / kamerthermostaat |
| 0x04 | SCB-17 | uitbreidingsprint (zones; bron van H10.xx) |
| 0x05 | GTW-Blue | interne BLE-gateway |

Zet `node:` per entity om een andere module dan 1 te bereiken.

---

## 9. Verbindings-state-machine (zo doet de component het)

De component doorloopt na elke (her)verbinding vaste stappen; ze volgen rechtstreeks
uit secties 2–5:

1. **OPEN** — vraag een versleutelde (gebonde) link met de passkey (sectie 2).
2. **Handles resolven** — zoek de GATT-handles van de data- en gate-characteristics.
3. **Notifies aanzetten** — op de betrokken characteristics; zodra alle CCCD's
   geschreven zijn, begint de handshake.
4. **Gate-handshake** — 0-byte write op 0x1B → challenge op 0x1D → TEA-antwoord op
   0x20 → ack op 0x22 (sectie 3.1).
5. **CANIP-handshake** — session-start + tweede TEA-challenge/response over de
   datakanalen (sectie 3.2).
6. **RUN** — normaal pollen: elke cyclus een GetList (bulk) plus eventuele losse
   reads, en writes zodra er een gevraagd wordt.

Valkuilen die direct uit het protocol volgen:

- Reassembleer op fragmenten; de seq-byte is géén berichtstart (4.1).
- De operatiebyte staat op offset [5] van het samengestelde bericht (4.2).
- Zet geen extra 0xFF vóór een read/write — die wordt genegeerd (4.2).
- Eén centrale tegelijk — ontkoppel de telefoon als de ESP verbindt (sectie 1).
