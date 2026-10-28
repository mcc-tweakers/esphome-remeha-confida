# Data — codeboek & object-woordenboek

Naslag bij de ESPHome-component: wat elk CAN-object betekent, hoe je het decodeert,
welke keuzes een enum-parameter heeft en welke foutcodes bestaan. Hiermee zoek je
objecten op als je zelf een sensor of parameter aan de YAML wilt toevoegen (zie de
README in de hoofdmap).

## Bestanden

| bestand | inhoud |
|---|---|
| **`Confida_parametercodeboek.xlsx`** | Het codeboek (3 tabbladen, met Nederlandse namen/uitleg). Zie hieronder. |

### Tabbladen in het codeboek

| tabblad | rijen | inhoud |
|---|---|---|
| **Confida** | 460 | De objecten die déze warmtepomp (EHC, node 1) daadwerkelijk gebruikt: 250 parameters, 155 signals, 55 counters. Afgeleid uit de GetList-set die het toestel zelf teruggeeft voor dit type (node 1), niet uit een index-heuristiek — dus dit dekt wat de ESP van dit toestel kan lezen. Kolom **In ESPHome** markeert wat de meegeleverde YAML al uitleest. |
| **Enums** | 1160 | Keuzelijsten (waarde → omschrijving NL/EN) voor enum-parameters. |
| **Foutcodes** | 1986 | De foutcode-catalogus in 24 categorieën, weergave `H{categorie}.{nummer}`. |

> **Het "Confida"-tabblad is niet per se compleet.** Het bevat de objecten uit de
> standaard GetList-set. Het toestel serveert echter óók objecten daarbuiten — die kun
> je alsnog los uitlezen (met een losse read). Voorbeeld:
> `CM260` (`0x5434`), een ruimtetemperatuur met hoge resolutie (0,01 i.p.v. 0,1 °C bij
> `CM030`), staat niet in de GetList-set maar wordt wél gevuld. Zulke extra objecten
> staan niet in dit codeboek; die vind je door zelf de BLE-log/scan van je toestel te
> bekijken — en controleer per object in de log of jouw toestel er een geldige waarde
> voor teruggeeft.

### Kolom "Soort"

`Soort` bepaalt of een object een instelling of een meetwaarde is, en daarmee of het
onder `sensor:` of `number:` in de YAML hoort: `Parameter` = instelbaar (`number`, of
alleen-lezen als `sensor`), `Signal` = live meetwaarde (`sensor`), `Counter` = oplopende
teller (`sensor` met `state_class: total_increasing`). Er is bewust géén "schrijfbaar"-
kolom: de alleen-lezen-vlag in het object-woordenboek markeert ook meetwaarden als
schrijfbaar en is dus onbetrouwbaar.

> De numerieke min/max/stap van parameters zitten niet in het object-woordenboek en
> komen ook niet over BLE; alleen de toegestane waarden van enum-parameters liggen vast.
