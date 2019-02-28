Этот демон предназначен для приема и записи в syslog trap'ов для утилиты
SmartConsole, применяемой для управления некоторыми моделями свитчей
D-Link. Некоторые из этих моделей (например DES-1100) не имеют других
функций оповещения (т.е. нет поддержки ни syslog, ни SNMP).

ВНИМАНИЕ! Не путать с SNMP-trap'ами!

Зависимости: библиотека PCRE (http://www.pcre.org/)

Установка и запуск под FreeBSD:
1) make all install (требуются права root)
2) добавить в /etc/rc.conf переменную `dlinktrapd_enable="YES"`,
   при необходимости `dlinktrapd_flags="ключи"`.

Если с системе внезапно не установлено pcre, то установить его из порта devel/pcre.


Компиляция под linux-base системами производится командой вида:
`gcc -Wall -O1 -lpcre -o dlinktrapd dlinktrapd.c`
Возможно, придеться явно задавать пути include и lib:
`gcc -Wall -O1 -I /usr/include -L /usr/lib -lpcre -o dlinktrapd dlinktrapd.c`

pcre ставится принятым в применяемом дистрибутиве образом, пример для
Debian "squeeze": `apt-get install libpcre3 libpcre3-dev`


Ключи командной строки:

-d: отладочный режим, не переходит в daemon mode, если указан два раза,
    то дополнительно показывает hex-дамп пакетов.
-i <IP>: listen address, по умолчанию слушает на всех интерфейсах, имеющих IP адреса.
-P <port>: listen port (default 64514)
-p <file>: pid file (default /var/run/dlinktrapd.pid)
-s: включает запись состояния портов свитча при получении trap'ов "port up"/"port down"
-f </some/dir/>: каталог для записи файла с состоянием портов. Имя файла - MAC устройства.


BUGS:
Сообщение, которое получено внутри trap'а, не проверяется на корректность и отправляется
в syslog в том виде, в котором пришло. Т.к. никаких следов проверки подлинности отправителя
в протоколе не обнаружено, то кто угодно под видом свитча может передать через syslog
привет админу длинной до 468 байт.


Формат сообщения;
марка_свитча пробел(ы) (код_сообщения)сообщение

Типы сообщений:
(1001)System bootup
(1002)WEB authenticate error from remote IP: xxx.xxx.xxx.xxx
(3003)Port X copper link up
(3004)Port X copper link down
(5001)Firmware upgraded success
(5002)Firmware upgraded failure
(5005)Wrong file checksum causes firmware upgrade failure.
