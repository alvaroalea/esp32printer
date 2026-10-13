# ESP32 Printer Emulator
for computers with serial RS232 port.

This is a vibecodding IA (claude) assited project.

This project is based on the hardware of fujinet-rs232
check the #defines at esp32printer.ino for the pinouts.

It provide a secondary serial port, including hardware handshake that emulate
a needle's colour printer type epson, as it accept a subset of the esc/p escape sequences.

It generate a .bmp and a .pdf files for each A4 size sheet printed.

Optionally the bmp file can be send to a network printer conected to the same wifi,
the printer need to be compatible with IPP Printing Everywhere, that is almost all of todays printers.

It's also provide a web server, it provide several functionality:
* you can see the ongoing page being printed
* you can configure the options of the printer and the IP address of the real printer.
* you can manage de sd card, see previous .bmp prints, download the .pdf version, send to a printer and delete it.

Still in development, only basic work.

by default is configured for a Sinclair QL Computer, 9600 bauds, 8N1 with Automatic CR on each LF configured, this can be changed on the configuration page.

## font folder
There are some small helpers written on python to convert and edit zx spectrum 768 bytes to font for printer, I use some ones from https://github.com/ZXSpectrumVault/zx-fonts to the diferents supported fonts.

I use this:
* courier - A Fistful of Blood Capsules (1987)(Zodiac Software)[end]#37966.ch8
* prestige - A Fistful of Blood Capsules (1987)(Zodiac Software)(Part 1 of 3)#37966.ch8
* roman - Fall of Rome, The (1984)(ASP Software)#56501.ch8
* script - Damned Forest, The (1988)(Cult Games)#64000.ch8
* OCR-A - Adventure F - The Eye of Bain (1984)(Artic Computing)#30263.ch8
* OCR-B - Alcatraz Harry 2 - The Doomsday Mission (1984)(Scorpio Gamesworld)#62602.ch8



## Led Status
the RGB led of the board indicate the state of the printer:

* RED - Error in access to SD
* GREEN - Papel is ejected, you can see the last printed in /current.bmp any new print will be in a new .bmp file
* BLUE - Printer waiting new command, press the FF button will cause the completation of the page and led go green. (when blue part of the lasted printed can be not show in the web, you need to print CR of FF)
* CYAN - Printer is working drawing the actual commands.
* PURPLE - Printer is receiving data and start to print.
* YELLOW - FF Button has been pressed and page is being "explused"

## Future works:

Printer Emulation TODO
* Proporcional font.
* Real NLQ fonts.
* Fonts loadables and in RAM.
* Country Setings, and slashed 0.
* User configurable fonts.
* configurable page side
* configurable nº of bit, parity and bit stop.

Tests TODO
* Verify with Windows and Linux drivers.
* Verify with BannerMania from MS-Dos.
* Verify in other systems, ZX Spectrum, Amstrad, Enterprise.

Other TODO
* Provide a configurable hostname
* be able to search other hostnames not only by IP
* Search and list available printers

## Actual Status
* Configurable Serial port and operation
* Support all text stiles from ESC/P
* Support for color selection
* Generate a .bmp and a .pdf file when page is complete
* Send page to a IPP Print Everywhere Printer (manual and automaticaly)
* Limited file management

These are examples of supported features:

Star LC 10 Colour basic demo from manual:
![My image](sample.png)

Colour dump of QL's game loading screen, with sdump from miracle: 
![My image](sample2.png)

Conected to a PC using Debian 13 (need AUTO_CR_ON_LF = false):
![My image](sample3.png)
