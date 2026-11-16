<div align="center">

# Half-Life Closed Captions

**Плагин субтитров (Closed Captions) для Half-Life 1 (25th Anniversary Update), Half-Life: Uplink и Half-Life: MMod.**

[![Engine](https://img.shields.io/badge/Engine-GoldSrc%20(1998)-orange?style=for-the-badge)](https://developer.valvesoftware.com/wiki/GoldSrc)
[![Graphics](https://img.shields.io/badge/Overlay-OpenGL-5586A4?style=for-the-badge&logo=opengl&logoColor=white)](https://www.opengl.org/)
[![Platform](https://img.shields.io/badge/Platform-Windows-0078D6?style=for-the-badge&logo=windows&logoColor=white)]()
[![License](https://img.shields.io/github/license/[USER]/[REPO]?style=for-the-badge&color=blue&label=License)](LICENSE)

[Скачать релиз](https://github.com/[USER]/[REPO]/releases) • [Сообщить об ошибке](https://github.com/[USER]/[REPO]/issues)

</div>

---

В оригинальном движке GoldSrc (1998) нет встроенной системы субтитров. Этот плагин решает проблему: он перехватывает звуковые события персонажей и окружения на уровне движка и отрисовывает текст поверх кадра через независимый OpenGL-оверлей используя словари (инжект через прокси-библиотеку `version.dll`).

## Возможности
- **Полное покрытие:** переведены на русский и английский сюжетные диалоги, реплики  и ключевые звуки окружения.
- **Гибкая кастомизация:** размер шрифта, цвета для разных персонажей и дистанция слышимости настраиваются в `subtitles.ini`.
- **Открытая база реплик:** все тексты хранятся в простых `.tsv`текстовых файлах, которые легко править или переводить на другие языки.

## Установка
1. Скачайте актуальный архив со страницы **[Releases](https://github.com/[USER]/[REPO]/releases)**.
2. Распакуйте содержимое в корневую папку игры (туда, где находится `hl.exe`).
3. Запустите игру — плагин подхватится автоматически.

> [!WARNING]
> **Внимание (VAC и мультиплеер):**  
> Плагин внедряется в память процесса и предназначен **строго для одиночной игры**. Перед заходом на мультиплеерные серверы с защитой VAC обязательно удалите или временно уберите `version.dll` из папки с игрой.

## В планах (Roadmap)
- [ ] Поддержка **Half-Life: Opposing Force** (`gearbox`)
- [ ] Поддержка **Half-Life: Blue Shift** (`bshift`)
