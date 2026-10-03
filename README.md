# ESP32 Printer Emulator
for computers with serial RS232 port.

This is a vibecodding IA assited project.

This project is based on the hardware of fujinet-rs232
check the #defines at esp32printer.ino for the pinouts.

It provide a secondary serial port, including hardware handshake that emulate
a needle's colour printer type epson, as it accept a subset of the esc/p escape sequences.

it generate a .bmp for each A4 size sheet printed.

It's also provide a web server, so can be reached by wifi and printed in a moderm printer using a modern computer and webbrowser.

Still in development, only basic work.

by default is configured for a Sinclair QL Computer, 9600 bauds, 8N1 with Automatic CR on each LF configured, this can be changed by #defines in the code.

## font folder
There are some small helpers written on python to convert and edit zx spectrum 768 bytes to font for printer, I use some ones from https://github.com/ZXSpectrumVault/zx-fonts to the diferents supported fonts.

## Led Status
the RGB led of the board indicate the state of the printer:

* RED - Error in access to SD
* GREEN - Papel is ejected, you can see the last printed in /current.bmp any new print will be in a new .bmp file
* BLUE - Printer waiting new command, press the FF button will cause the completation of the page and led go green. (when blue part of the lasted printed can be not show in the web, you need to print CR of FF)
* CYAN - Printer is working drawing the actual commands.
* PURPLE - Printer is receiving data and start to print.
* YELLOW - FF Button has been pressed and page is being "explused"

## Actual Status
This is a example of supported features:

![My image](sample.png) 
