# GoldSrc Closed Captions (Half-Life 1 Subtitles Engine)

[🇬🇧 English](#english) | [🇷🇺 Русский](#русский)

---

## English

Plugin Closed Captions & Subtitles engine for **Half-Life 1 (25th Anniversary Update)**, **Half-Life: Uplink**, and **Half-Life: MMod**, powered by a standalone OpenGL texture-atlas overlay and proxy `version.dll`.

Since the original 1998 GoldSrc engine has no built-in subtitle system, this plugin hooks audio playback directly in the engine and renders crisp, color-coded captions on top of the frame.

### Installation
1. Download the latest **RU** or **EN** archive from the **[Releases](https://github.com/grvelvet/GoldSrc-Closed-Captions/releases/latest)** page.
2. Extract the archive contents directly into your `Half-Life` root directory (next to `hl.exe`).
3. Launch the game and play.

> **⚠️ VAC Warning (Single-Player Only):**  
> This plugin hooks engine functions in memory and is intended **strictly for single-player**. Temporarily remove `version.dll` from the game folder before joining VAC-secured multiplayer servers.

### Roadmap
* **Half-Life: Opposing Force** (`gearbox`) support
* **Half-Life: Blue Shift** (`bshift`) support

---

## Русский
Плагин движка субтитров (Closed Captions) для **Half-Life 1 (25th Anniversary Update)**, **Half-Life: Uplink** и **Half-Life: MMod** на базе независимого OpenGL-оверлея и прокси-библиотеки `version.dll`[cite: 1].

В оригинальном движке GoldSrc (1998) отсутствует полностью встроенный механизм субтитров, поэтому плагин перехватывает реплики персонажей и окружения на уровне движка и выводит чёткие субтитры  поверх кадра.

### Возможности
* **Две готовые языковые сборки:** отдельные независимые архивы для русской (RU) и английской (EN) версий.
* **Полное покрытие реплик:** 
* **Чёткий шрифт в любом разрешении:** 
* **Гибкая настройка:** размер шрифта, цвета ролей и дистанция слышимости настраиваются в `subtitles.ini`, а базы реплик хранятся в открытых `.tsv`-таблицах[cite: 1].

### Установка
1. Скачайте актуальный архив (**RU** или **EN**) со страницы **[Releases](https://github.com/grvelvet/GoldSrc-Closed-Captions/releases/latest)**.
2. Распакуйте содержимое архива в корневую папку `Half-Life` (рядом с `hl.exe`).
3. Запускайте игру — всё работает сразу после распаковки.

> **⚠️ Важно (только для одиночной игры / VAC):**  
> Плагин перехватывает функции движка в памяти процесса и предназначен **строго для одиночной кампании**. Перед входом на мультиплеерные серверы с защитой **VAC** обязательно уберите `version.dll` из папки с игрой.

### В планах (Roadmap)
* Поддержка **Half-Life: Opposing Force** (`gearbox`)
* Поддержка **Half-Life: Blue Shift** (`bshift`)
