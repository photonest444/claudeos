/* UI translations. The source code is written in English; T() returns the
 * Russian text when Settings -> Language is "Русский". Strings are UTF-8 and
 * the font contains the Cyrillic alphabet. Text that is not in the table
 * (file names, numbers, program output) is shown unchanged. */
#include "../include/gui.h"
#include "../include/string.h"

static const struct { const char *en, *ru; } dict[] = {
    /* apps and window titles */
    { "Terminal", "Терминал" }, { "Files", "Файлы" }, { "Notes", "Заметки" },
    { "Calculator", "Калькулятор" }, { "Monitor", "Монитор" }, { "Widgets", "Виджеты" },
    { "Snake", "Змейка" }, { "Settings", "Настройки" }, { "About", "О системе" },
    { "Widgets Demo", "Демо виджетов" }, { "System Monitor", "Системный монитор" },
    { "About ClaudeOS", "О ClaudeOS" }, { "Terminal (pid %d)", "Терминал (pid %d)" },
    /* launcher, menus, clipboard */
    { "Console", "Консоль" }, { "Restart", "Перезапуск" }, { "Power off", "Выключить" },
    { "Restore", "Развернуть" }, { "Minimize", "Свернуть" }, { "Close", "Закрыть" },
    { "Close window", "Закрыть окно" }, { "New terminal", "Новый терминал" },
    { "Next wallpaper", "Следующие обои" }, { "Dark theme", "Тёмная тема" },
    { "Light theme", "Светлая тема" }, { "Cut", "Вырезать" }, { "Copy", "Копировать" },
    { "Paste", "Вставить" }, { "Select all", "Выделить всё" }, { "Delete", "Удалить" },
    { "Open", "Открыть" }, { "Open in Notes", "Открыть в Заметках" }, { "New folder", "Новая папка" },
    { "Refresh", "Обновить" }, { "Up", "Вверх" }, { "Save", "Сохранить" }, { "Load", "Загрузить" },
    { "New", "Новый" }, { "Today", "Сегодня" }, { "Uptime %u:%02u", "Работает %u:%02u" },
    { "Cut to clipboard", "Вырезано в буфер обмена" }, { "Copied to clipboard", "Скопировано в буфер обмена" },
    { "Clipboard is empty", "Буфер обмена пуст" },
    { "Welcome! Press F12 to return to the console.", "Добро пожаловать! F12 - вернуться в консоль." },
    { "Not enough memory for a new window", "Недостаточно памяти для нового окна" },
    { "Too many windows open", "Открыто слишком много окон" },
    { "Could not save settings to /etc/gui.cfg", "Не удалось сохранить настройки в /etc/gui.cfg" },
    /* calculator */
    { "Error", "Ошибка" },
    /* widgets demo */
    { "Text input", "Ввод текста" }, { "Type your name...", "Введите имя..." }, { "Greet", "Привет" },
    { "Type a name and press Enter.", "Введите имя и нажмите Enter." },
    { "Hello, %s! Welcome to ClaudeOS.", "Привет, %s! Добро пожаловать в ClaudeOS." },
    { "Please type your name first.", "Сначала введите имя." }, { "Controls", "Элементы управления" },
    { "Click me", "Нажми меня" }, { "Clicked: %d", "Нажатий: %d" }, { "Clicked: 0", "Нажатий: 0" },
    { "Notifications", "Уведомления" }, { "Notifications enabled", "Уведомления включены" },
    { "Notifications disabled", "Уведомления выключены" }, { "Volume", "Громкость" }, { "Size", "Размер" },
    { "Small", "Малый" }, { "Medium", "Средний" }, { "Large", "Большой" }, { "Size: %s", "Размер: %s" },
    { "Write a note...", "Напишите заметку..." }, { "Save failed: %s", "Ошибка сохранения: %s" },
    { "Saved %d bytes to %s", "Сохранено %d байт в %s" }, { "Load failed: %s", "Ошибка загрузки: %s" },
    { "Loaded %d bytes from %s", "Загружено %d байт из %s" },
    { "Tab moves focus, Space/arrows change controls.", "Tab - фокус, пробел и стрелки меняют значения." },
    /* system monitor */
    { "Uptime", "Время работы" }, { "CPU", "ЦП" }, { "Tasks", "Задачи" },
    { "CPU usage, last 30 s", "Загрузка ЦП за 30 с" }, { "RAM  %u / %u MB", "ОЗУ  %u / %u МБ" },
    { "Heap  %u / %u KB", "Куча  %u / %u КБ" },
    { "PID  STATE      CPU(ms)  TYPE    NAME", "PID  Сост.       ЦП(мс)  Тип     Имя" },
    /* about */
    { "Version 1.2 for i386", "Версия 1.2 для i386" },
    { "C + NASM kernel, preemptive multitasking,", "Ядро на C + NASM, вытесняющая многозадачность," },
    { "FAT12/16, ring-3 programs and this desktop.", "FAT12/16, программы в ring 3 и этот рабочий стол." },
    { "Font: Terminus (SIL Open Font License)", "Шрифт: Terminus (SIL Open Font License)" },
    /* files */
    { "%d item(s)  -  right-click for actions", "Объектов: %d  -  правый клик для действий" },
    { "[folder]\n\nPress Enter or click it again to open.", "[папка]\n\nНажмите Enter или щёлкните ещё раз, чтобы открыть." },
    { "[binary file, %u bytes]\n\nRun it from a terminal:\n  %s", "[двоичный файл, %u байт]\n\nЗапустите его в терминале:\n  %s" },
    { "%s - %u bytes", "%s - %u байт" }, { "Select a file to copy", "Выберите файл для копирования" },
    { "File copied - paste it with Ctrl+V", "Файл скопирован - вставьте его Ctrl+V" },
    { "Nothing to paste", "Нечего вставлять" }, { "Pasted as %s", "Вставлено как %s" },
    { "Deleted %s", "Удалено: %s" }, { "Folder created", "Папка создана" },
    { "Select a file to preview it.", "Выберите файл для просмотра." },
    /* settings */
    { "Settings restored to defaults", "Настройки сброшены" }, { "Appearance", "Внешний вид" },
    { "Theme", "Тема" }, { "Dark", "Тёмная" }, { "Light", "Светлая" }, { "Language", "Язык" },
    { "Accent color", "Акцентный цвет" }, { "Wallpaper", "Обои" }, { "Effects", "Эффекты" },
    { "Animations", "Анимации" }, { "Animation speed", "Скорость анимаций" }, { "Slow", "Медленно" },
    { "Normal", "Обычно" }, { "Fast", "Быстро" }, { "Window shadows", "Тени окон" },
    { "Translucent panels", "Прозрачные панели" }, { "Desktop", "Рабочий стол" },
    { "Clock shows seconds", "Секунды в часах" }, { "Mouse speed", "Скорость мыши" },
    { "Reset to defaults", "Сбросить" }, { "Saved to /etc/gui.cfg", "Сохраняется в /etc/gui.cfg" },
    /* notes */
    { "Select some text first (Ctrl+A selects all)", "Сначала выделите текст (Ctrl+A - всё)" },
    { "Type a file name, e.g. /home/notes.txt", "Введите имя файла, например /home/notes.txt" },
    { "Opened %s", "Открыт %s" }, { "Line %d, column %d   |   %d characters", "Строка %d, столбец %d   |   символов: %d" },
    { "modified - Ctrl+S saves", "изменён - Ctrl+S сохраняет" },
    /* snake */
    { "New record!", "Новый рекорд!" }, { "Score: %d", "Очки: %d" }, { "Best: %d", "Рекорд: %d" },
    { "Press Space to start", "Нажмите пробел, чтобы начать" },
    { "Game over - Space to play again", "Игра окончена - пробел, чтобы сыграть снова" },
    { "Paused - Space to continue", "Пауза - пробел, чтобы продолжить" },
    { "Arrows or WASD to steer", "Управление: стрелки или WASD" },
};

const char *T(const char *en) {
    if (!en || !gui_settings.lang) return en;
    for (unsigned i = 0; i < sizeof(dict) / sizeof(dict[0]); i++)
        if (!strcmp(dict[i].en, en)) return dict[i].ru;
    return en;
}
