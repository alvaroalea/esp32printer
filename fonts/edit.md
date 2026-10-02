### Un detalle importante sobre el formato

El programa mantiene exactamente la organización que hemos establecido: **cada carácter está formado por `ancho × alto_bytes` bytes**, siendo cada byte una columna de 8 píxeles.

Con la configuración predeterminada:

```text
Ancho = 8 bytes
Alto  = 1 byte = 8 píxeles
```

la cuadrícula es:

```text
       8 columnas
    ┌─┬─┬─┬─┬─┬─┬─┬─┐
  0 │ │ │ │ │ │ │ │ │
  1 │ │ │ │ │ │ │ │ │
  2 │ │ │ │ │ │ │ │ │
  3 │ │ │ │ │ │ │ │ │
  4 │ │ │ │ │ │ │ │ │
  5 │ │ │ │ │ │ │ │ │
  6 │ │ │ │ │ │ │ │ │
  7 │ │ │ │ │ │ │ │ │
    └─┴─┴─┴─┴─┴─┴─┴─┘
       8 píxeles
```

Y el bit correspondiente es:

```text
byte 0 ── columna 0
  bit 0 ── píxel Y=0
  bit 1 ── píxel Y=1
  ...
  bit 7 ── píxel Y=7

byte 1 ── columna 1
...
byte 7 ── columna 7
```

Por tanto, **lo que ves en pantalla corresponde directamente a los bits que acabamos de generar con el conversor anterior**.

Hay una cosa que mejoraría en una siguiente versión: actualmente cambiar el ancho/alto reorganiza los bytes existentes. Si lo que quieres es utilizar, por ejemplo, **24×24 píxeles como un único carácter de 72 bytes**, sería mejor que el editor tratase explícitamente esos bloques como caracteres independientes y conservara también el número de caracteres del fichero.
