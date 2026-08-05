<div align="center">

# Surrealra1nForge

**Mój osobisty, eksperymentalny fork projektu [surrealra1n](https://github.com/pwnerblu/surrealra1n)**

[![Platforma](https://img.shields.io/badge/platforma-macOS%20%7C%20Linux-lightgrey?style=flat-square)](#wymagania)
[![Licencja](https://img.shields.io/badge/licencja-Apache%202.0-blue?style=flat-square)](LICENSE)
[![Status](https://img.shields.io/badge/status-eksperymentalny-orange?style=flat-square)](#ważne)

**Polski** · [English](README.md)

</div>

## O projekcie

Surrealra1nForge jest moim osobistym forkiem projektu
[surrealra1n](https://github.com/pwnerblu/surrealra1n). Używam go do dodawania
i testowania różnych funkcji oraz opcji — w tym eksperymentalnych — które mogą
mi się przydać w pracy, badaniach i testowaniu urządzeń.

Niektóre zmiany mogą być przeznaczone do bardzo konkretnych zastosowań i słabiej
przetestowane niż projekt bazowy. Obecnie skupiam się głównie na urządzeniach
A12 i A13, chociaż Forge nie ogranicza się wyłącznie do tych platform.

> [!IMPORTANT]
> Jeśli potrzebujesz wyłącznie standardowych funkcji surrealra1n, polecam
> skorzystać z [oryginalnego projektu](https://github.com/pwnerblu/surrealra1n).

## Pochodzenie i podziękowania

Surrealra1nForge nie powstałby bez
[**PWNBlue**](https://github.com/pwnerblu), twórcy oryginalnego projektu
[surrealra1n](https://github.com/pwnerblu/surrealra1n). Dziękuję PWNBlue oraz
wszystkim współtwórcom upstreamu za stworzenie i rozwijanie podstaw, dzięki
którym ten fork może istnieć.

## Dodatki Forge

Forge obecnie dodaje lub rozwija następujące funkcje:

- **System Mods** — eksperymentalne modyfikacje SSV dla iOS 15 i iOS 16,
  w tym **SSH Dropbear (iOS 15+)**.
- **Custom Binpatcher** — konfigurowalne patche bajtowe plików binarnych
  z selektorami wersji i kontrolą bezpieczeństwa.
- **Eksperymenty A12/A13** — zmiany dotyczące przywracania, patchowania
  i testowania urządzeń, rozwijane głównie z myślą o sprzęcie A12 i A13.

Dostępność funkcji zależy od urządzenia, wersji iOS i wybranej konfiguracji
przywracania. Własnymi patchami binarnymi można zarządzać w menu **System
Patches Config → Custom Binpatches Configurator**. Ich formaty, selektory,
zabezpieczenia i dwuetapowy proces przywracania opisuje
[dokumentacja Custom Binpatcher](custom_binpatcher/README.md).

## Wymagania

- macOS lub Linux
- obsługiwane urządzenie i wersja iOS
- zależności wskazane przez skrypt

Projekt bazowy obsługuje przywracanie kilku rodzin urządzeń. Informacje o jego
kompatybilności znajdziesz w oryginalnej
[wiki Supported Devices](https://github.com/pwnerblu/surrealra1n/wiki/Supported-Devices).

## Instalacja

Sklonuj repozytorium wraz z submodułami i uruchom skrypt:

```sh
git clone --branch development --recursive https://github.com/sthomsonpl/Surrealra1nForge.git
cd Surrealra1nForge
./surrealra1n.sh
```

Skrypt zachowuje oryginalną nazwę `surrealra1n.sh` ze względu na kompatybilność.
Nie uruchamiaj go bezpośrednio jako root ani przez `sudo`.

## Ważne

> [!CAUTION]
> Forge zawiera eksperymentalne funkcje. Czytaj każde ostrzeżenie wyświetlane
> przez skrypt, zachowuj pełne logi terminala i upewnij się, że rozumiesz
> wybrane opcje przed rozpoczęciem przywracania.

Fork jest rozwijany niezależnie i nie stanowi oficjalnego wydania projektu
bazowego.

## Podziękowania

- [**PWNBlue**](https://github.com/pwnerblu) — twórca oryginalnego projektu
  [surrealra1n](https://github.com/pwnerblu/surrealra1n), na którym bazuje ten
  fork.
- Zespół libimobiledevice, tihmstar, LukeeGD/LukeZGD, xerub, plooshi oraz
  pozostali autorzy narzędzi używanych przez projekt.
- Mineek — patcher restored dla iPhone'a X, openra1n i seprmvr64.
- Nathan (verygenericname) — SSHRD_Script.
- bodyc1m — obsługa iPoda touch 6 oraz port dla Arch Linux/Fedory.

## Licencja

Surrealra1nForge jest dostępny na licencji Apache License 2.0. Zobacz
[LICENSE](LICENSE) oraz [NOTICE](NOTICE).
