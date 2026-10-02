#!/usr/bin/env python3

import tkinter as tk
from tkinter import filedialog, messagebox
import re
import os


class CharacterEditor(tk.Tk):

    def __init__(self):
        super().__init__()

        self.title("Editor de caracteres gráficos")
        self.geometry("950x780")
        self.minsize(700, 550)

        # --------------------------------------------------------
        # Archivo
        # --------------------------------------------------------

        self.filename = None
        self.file_data = ""

        self.array_start = None
        self.array_end = None

        self.array_prefix = ""
        self.array_suffix = ""

        # Bytes originales
        self.raw_bytes = []

        # Lista de caracteres
        self.characters = []

        self.current_char = 0

        # --------------------------------------------------------
        # Dimensiones
        # --------------------------------------------------------

        self.char_width = 8
        self.char_height_bytes = 1

        # --------------------------------------------------------
        # Construcción de interfaz
        # --------------------------------------------------------

        self.build_ui()

        self.create_grid()

    # ============================================================
    # INTERFAZ
    # ============================================================

    def build_ui(self):

        # --------------------------------------------------------
        # Archivo
        # --------------------------------------------------------

        frame_file = tk.Frame(self)
        frame_file.pack(
            fill="x",
            padx=8,
            pady=8
        )

        tk.Button(
            frame_file,
            text="Abrir",
            width=12,
            command=self.open_file
        ).pack(
            side="left",
            padx=3
        )

        tk.Button(
            frame_file,
            text="Guardar",
            width=12,
            command=self.save_file
        ).pack(
            side="left",
            padx=3
        )

        tk.Button(
            frame_file,
            text="Guardar como...",
            width=15,
            command=self.save_as
        ).pack(
            side="left",
            padx=3
        )

        # --------------------------------------------------------
        # Dimensiones
        # --------------------------------------------------------

        frame_dim = tk.LabelFrame(
            self,
            text="Dimensiones del carácter"
        )

        frame_dim.pack(
            fill="x",
            padx=8,
            pady=4
        )

        tk.Label(
            frame_dim,
            text="Ancho:"
        ).pack(
            side="left",
            padx=(10, 3)
        )

        self.width_var = tk.IntVar(
            value=8
        )

        self.width_spin = tk.Spinbox(
            frame_dim,
            from_=2,
            to=24,
            width=5,
            textvariable=self.width_var
        )

        self.width_spin.pack(
            side="left"
        )

        tk.Label(
            frame_dim,
            text="bytes"
        ).pack(
            side="left",
            padx=(3, 20)
        )

        tk.Label(
            frame_dim,
            text="Alto:"
        ).pack(
            side="left"
        )

        self.height_var = tk.IntVar(
            value=1
        )

        self.height_spin = tk.Spinbox(
            frame_dim,
            from_=1,
            to=3,
            width=5,
            textvariable=self.height_var
        )

        self.height_spin.pack(
            side="left"
        )

        tk.Label(
            frame_dim,
            text="bytes"
        ).pack(
            side="left",
            padx=(3, 20)
        )

        tk.Button(
            frame_dim,
            text="Aplicar",
            command=self.apply_dimensions
        ).pack(
            side="left",
            padx=5
        )

        # --------------------------------------------------------
        # Navegación
        # --------------------------------------------------------

        frame_nav = tk.LabelFrame(
            self,
            text="Carácter"
        )

        frame_nav.pack(
            fill="x",
            padx=8,
            pady=4
        )

        tk.Button(
            frame_nav,
            text="◀ Anterior",
            width=12,
            command=self.previous_character
        ).pack(
            side="left",
            padx=5,
            pady=5
        )

        tk.Button(
            frame_nav,
            text="Siguiente ▶",
            width=12,
            command=self.next_character
        ).pack(
            side="left",
            padx=5
        )

        tk.Button(
            frame_nav,
            text="+ Añadir carácter",
            width=17,
            command=self.add_character
        ).pack(
            side="left",
            padx=15
        )

        tk.Label(
            frame_nav,
            text="ASCII:"
        ).pack(
            side="left",
            padx=(20, 3)
        )

        self.char_var = tk.IntVar(
            value=32
        )

        self.char_spin = tk.Spinbox(
            frame_nav,
            from_=0,
            to=255,
            width=6,
            textvariable=self.char_var
        )

        self.char_spin.pack(
            side="left"
        )

        tk.Button(
            frame_nav,
            text="Ir",
            command=self.goto_character
        ).pack(
            side="left",
            padx=5
        )

        self.character_label = tk.Label(
            frame_nav,
            text="32 ' '",
            font=("TkDefaultFont", 12, "bold")
        )

        self.character_label.pack(
            side="left",
            padx=15
        )

        # --------------------------------------------------------
        # Canvas
        # --------------------------------------------------------

        frame_canvas = tk.Frame(self)

        frame_canvas.pack(
            fill="both",
            expand=True,
            padx=8,
            pady=8
        )

        self.canvas = tk.Canvas(
            frame_canvas,
            background="white",
            highlightthickness=1,
            highlightbackground="gray"
        )

        self.canvas.pack(
            fill="both",
            expand=True
        )

        self.canvas.bind(
            "<Button-1>",
            self.pixel_click
        )

        # --------------------------------------------------------
        # Estado
        # --------------------------------------------------------

        self.status_var = tk.StringVar(
            value="Abra un archivo C/H"
        )

        tk.Label(
            self,
            textvariable=self.status_var,
            anchor="w",
            relief="sunken"
        ).pack(
            fill="x",
            side="bottom"
        )

        self.bind(
            "<Configure>",
            self.window_resized
        )

    # ============================================================
    # DETECCIÓN DE FONT..._COLS
    # ============================================================

    def detect_font_cols(self, text):

        """
        Busca definiciones como:

            #define FONT2_COLS 5
            #define FONTdos_COLS 8

        El nombre debe empezar por FONT y terminar en _COLS.
        """

        pattern = re.compile(
            r'^\s*#\s*define\s+'
            r'(FONT[A-Za-z0-9_]*_COLS)'
            r'\s+(\d+)',
            re.MULTILINE
        )

        matches = pattern.findall(text)

        if not matches:
            return None

        # Si hay varios, utilizar el primero.
        for name, value in matches:

            width = int(value)

            if 2 <= width <= 24:
                return width

        return None

    # ============================================================
    # DETECCIÓN DEL ARRAY
    # ============================================================

    def find_array(self, text):

        """
        Reconoce arrays como:

            static const uint8_t charset[768] = {

        o:

            static const uint8_t font8x8[] PROGMEM = {

        También acepta:

            uint8_t foo[] = {

            const unsigned char foo[] = {

        """

        pattern = re.compile(
            r'''
            (?P<prefix>
                (?:static\s+)?
                (?:const\s+)?
                (?:volatile\s+)?
                (?:
                    uint8_t
                    |
                    unsigned\s+char
                    |
                    signed\s+char
                    |
                    char
                )
                \s+
                (?P<name>[A-Za-z_]\w*)
                \s*
                \[
                    [^\]]*
                \]
                (?:
                    \s+[A-Za-z_]\w*
                )*
                \s*=\s*\{
            )
            ''',
            re.VERBOSE
        )

        match = pattern.search(text)

        if not match:
            return None

        start = match.end()

        # Buscar el cierre del array.
        # Se utiliza el primer }; posterior.
        end_match = re.search(
            r'\};',
            text[start:]
        )

        if not end_match:
            return None

        end = (
            start +
            end_match.start()
        )

        return (
            start,
            end,
            match.group("name")
        )

    # ============================================================
    # ABRIR
    # ============================================================

    def open_file(self):

        filename = filedialog.askopenfilename(
            title="Abrir array C",
            filetypes=[
                ("Archivos C", "*.c"),
                ("Cabeceras C", "*.h"),
                ("Todos los archivos", "*.*")
            ]
        )

        if not filename:
            return

        try:

            with open(
                filename,
                "r",
                encoding="utf-8"
            ) as f:

                text = f.read()

        except Exception as e:

            messagebox.showerror(
                "Error",
                f"No se pudo abrir el archivo:\n\n{e}"
            )

            return

        # --------------------------------------------------------
        # Detectar ancho definido mediante FONT..._COLS
        # --------------------------------------------------------

        detected_width = (
            self.detect_font_cols(text)
        )

        if detected_width is not None:

            self.char_width = detected_width

            self.width_var.set(
                detected_width
            )

        else:

            self.char_width = (
                self.width_var.get()
            )

        # --------------------------------------------------------
        # Buscar array
        # --------------------------------------------------------

        result = self.find_array(text)

        if result is None:

            messagebox.showerror(
                "Error",
                "No se encontró ningún array de "
                "bytes válido."
            )

            return

        (
            array_start,
            array_end,
            array_name
        ) = result

        array_text = text[
            array_start:array_end
        ]

        # --------------------------------------------------------
        # Extraer bytes
        # --------------------------------------------------------

        values = re.findall(
            r'(?<![A-Za-z0-9_])'
            r'0[xX]([0-9A-Fa-f]{1,2})'
            r'(?![A-Za-z0-9_])',
            array_text
        )

        if not values:

            messagebox.showerror(
                "Error",
                "El array no contiene valores "
                "hexadecimales."
            )

            return

        self.raw_bytes = [
            int(value, 16)
            for value in values
        ]

        # --------------------------------------------------------
        # Guardar información del fichero
        # --------------------------------------------------------

        self.filename = filename
        self.file_data = text

        self.array_start = array_start
        self.array_end = array_end

        # --------------------------------------------------------
        # Crear caracteres
        # --------------------------------------------------------

        self.rebuild_characters()

        self.current_char = 0

        self.update_display()

        self.status_var.set(
            f"{os.path.basename(filename)} | "
            f"array '{array_name}' | "
            f"{len(self.raw_bytes)} bytes | "
            f"{len(self.characters)} caracteres | "
            f"ancho {self.char_width}"
        )

    # ============================================================
    # RECONSTRUIR CARACTERES
    # ============================================================

    def rebuild_characters(self):

        width = self.char_width
        height = self.char_height_bytes

        char_size = (
            width * height
        )

        self.characters = []

        if char_size <= 0:
            return

        for pos in range(
            0,
            len(self.raw_bytes),
            char_size
        ):

            block = self.raw_bytes[
                pos:pos + char_size
            ]

            if len(block) < char_size:

                block += [0] * (
                    char_size - len(block)
                )

            self.characters.append(
                block
            )

    # ============================================================
    # AÑADIR CARÁCTER
    # ============================================================

    def add_character(self):

        width = self.char_width
        height = self.char_height_bytes

        char_size = (
            width * height
        )

        # Crear carácter vacío
        new_char = [
            0
            for _ in range(char_size)
        ]

        self.characters.append(
            new_char
        )

        # Añadir también a raw_bytes
        self.raw_bytes.extend(
            new_char
        )

        # Seleccionar el nuevo carácter
        self.current_char = (
            len(self.characters) - 1
        )

        self.update_display()

        ascii_code = (
            32 + self.current_char
        )

        self.status_var.set(
            f"Nuevo carácter añadido | "
            f"posición {self.current_char} | "
            f"ASCII {ascii_code}"
        )

    # ============================================================
    # GUARDAR
    # ============================================================

    def save_file(self):

        if self.filename is None:

            self.save_as()

        else:

            self.write_file(
                self.filename
            )

    # ============================================================

    def save_as(self):

        filename = filedialog.asksaveasfilename(
            title="Guardar archivo",
            defaultextension=".c",
            filetypes=[
                ("Archivos C", "*.c"),
                ("Cabeceras C", "*.h"),
                ("Todos los archivos", "*.*")
            ]
        )

        if not filename:
            return

        self.write_file(
            filename
        )

    # ============================================================
    # ESCRIBIR ARCHIVO
    # ============================================================

    def write_file(self, filename):

        if not self.characters:
            return

        width = self.char_width
        height = self.char_height_bytes

        char_size = (
            width * height
        )

        lines = []

        for char_number, char_data in enumerate(
            self.characters
        ):

            data = char_data[
                :char_size
            ]

            ascii_code = (
                32 + char_number
            )

            # ----------------------------------------------------
            # Comentario ASCII
            # ----------------------------------------------------

            if 32 <= ascii_code <= 126:

                character = chr(
                    ascii_code
                )

                if character == "'":
                    character = "\\'"

                comment = (
                    f"// {ascii_code} "
                    f"'{character}'"
                )

            else:

                comment = (
                    f"// {ascii_code} '?'"
                )

            values = ", ".join(
                f"0x{value:02X}"
                for value in data
            )

            # Coma excepto en el último
            if char_number < (
                len(self.characters) - 1
            ):

                comma = ","

            else:

                comma = ""

            lines.append(
                f"    {values}{comma}  {comment}"
            )

        new_array = "\n".join(
            lines
        )

        new_text = (
            self.file_data[
                :self.array_start
            ]
            + "\n"
            + new_array
            + "\n"
            + self.file_data[
                self.array_end:
            ]
        )

        try:

            with open(
                filename,
                "w",
                encoding="utf-8"
            ) as f:

                f.write(new_text)

        except Exception as e:

            messagebox.showerror(
                "Error",
                f"No se pudo guardar:\n\n{e}"
            )

            return

        self.filename = filename
        self.file_data = new_text

        # Recalcular posiciones
        result = self.find_array(
            new_text
        )

        if result:

            (
                self.array_start,
                self.array_end,
                _
            ) = result

        self.status_var.set(
            f"Guardado: {filename}"
        )

    # ============================================================
    # DIMENSIONES
    # ============================================================

    def apply_dimensions(self):

        try:

            width = int(
                self.width_var.get()
            )

            height = int(
                self.height_var.get()
            )

        except ValueError:

            messagebox.showerror(
                "Error",
                "Las dimensiones deben ser números."
            )

            return

        if not 2 <= width <= 24:

            messagebox.showerror(
                "Error",
                "El ancho debe estar entre "
                "2 y 24 bytes."
            )

            return

        if not 1 <= height <= 3:

            messagebox.showerror(
                "Error",
                "El alto debe estar entre "
                "1 y 3 bytes."
            )

            return

        # Mantener los bytes existentes
        # y reorganizarlos.
        old_bytes = []

        for char in self.characters:
            old_bytes.extend(char)

        self.char_width = width
        self.char_height_bytes = height

        if old_bytes:

            self.raw_bytes = old_bytes

        self.rebuild_characters()

        if self.current_char >= len(
            self.characters
        ):

            self.current_char = max(
                0,
                len(self.characters) - 1
            )

        self.update_display()

    # ============================================================
    # CUADRÍCULA
    # ============================================================

    def create_grid(self):

        self.canvas.delete(
            "all"
        )

        width = self.char_width

        height = (
            self.char_height_bytes * 8
        )

        canvas_width = max(
            self.canvas.winfo_width(),
            400
        )

        canvas_height = max(
            self.canvas.winfo_height(),
            300
        )

        cell_x = (
            canvas_width - 40
        ) // width

        cell_y = (
            canvas_height - 40
        ) // height

        self.cell_size = max(
            8,
            min(
                cell_x,
                cell_y,
                50
            )
        )

        total_width = (
            width * self.cell_size
        )

        total_height = (
            height * self.cell_size
        )

        self.canvas.config(
            scrollregion=(
                0,
                0,
                total_width,
                total_height
            )
        )

    # ============================================================
    # DIBUJAR
    # ============================================================

    def update_display(self):

        self.create_grid()

        if not self.characters:
            return

        if not (
            0 <= self.current_char
            < len(self.characters)
        ):
            return

        data = self.characters[
            self.current_char
        ]

        width = self.char_width

        height = (
            self.char_height_bytes * 8
        )

        size = self.cell_size

        for y in range(height):

            byte_row = (
                y // 8
            )

            bit = (
                y % 8
            )

            for x in range(width):

                index = (
                    byte_row * width
                    + x
                )

                value = data[index]

                pixel = (
                    value >> bit
                ) & 1

                x0 = x * size
                y0 = y * size
                x1 = x0 + size
                y1 = y0 + size

                self.canvas.create_rectangle(
                    x0,
                    y0,
                    x1,
                    y1,
                    fill=(
                        "black"
                        if pixel
                        else "white"
                    ),
                    outline="gray"
                )

        # --------------------------------------------------------
        # Información
        # --------------------------------------------------------

        ascii_code = (
            32 + self.current_char
        )

        if 32 <= ascii_code <= 126:
            character = chr(
                ascii_code
            )
        else:
            character = "?"

        self.char_var.set(
            ascii_code
        )

        self.character_label.config(
            text=f"{ascii_code} '{character}'"
        )

    # ============================================================
    # PIXEL
    # ============================================================

    def pixel_click(self, event):

        if not self.characters:
            return

        size = self.cell_size

        x = int(
            event.x // size
        )

        y = int(
            event.y // size
        )

        width = self.char_width

        height = (
            self.char_height_bytes * 8
        )

        if not (
            0 <= x < width
            and 0 <= y < height
        ):
            return

        byte_row = (
            y // 8
        )

        bit = (
            y % 8
        )

        index = (
            byte_row * width
            + x
        )

        self.characters[
            self.current_char
        ][index] ^= (
            1 << bit
        )

        # Actualizar raw_bytes
        absolute_index = (
            self.current_char
            * width
            * self.char_height_bytes
            + index
        )

        if absolute_index < len(
            self.raw_bytes
        ):

            self.raw_bytes[
                absolute_index
            ] = self.characters[
                self.current_char
            ][index]

        self.update_display()

    # ============================================================
    # NAVEGACIÓN
    # ============================================================

    def previous_character(self):

        if not self.characters:
            return

        if self.current_char > 0:

            self.current_char -= 1

            self.update_display()

    # ============================================================

    def next_character(self):

        if not self.characters:
            return

        if self.current_char < (
            len(self.characters) - 1
        ):

            self.current_char += 1

            self.update_display()

    # ============================================================

    def goto_character(self):

        try:

            ascii_code = int(
                self.char_var.get()
            )

        except ValueError:

            return

        index = (
            ascii_code - 32
        )

        if 0 <= index < len(
            self.characters
        ):

            self.current_char = index

            self.update_display()

    # ============================================================
    # REDIMENSIONADO DE VENTANA
    # ============================================================

    def window_resized(self, event):

        if event.widget is self:

            if hasattr(
                self,
                "canvas"
            ):

                # Evitar reconstruir datos.
                self.create_grid()

                if self.characters:
                    self.update_display()


# ================================================================
# MAIN
# ================================================================

if __name__ == "__main__":

    app = CharacterEditor()

    app.mainloop()
