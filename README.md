# ClaudeOS

Ein eigenes 64-Bit-Betriebssystem für x86_64, komplett in C (plus etwas NASM-Assembler) geschrieben –
mit grafischem Desktop, Taskleiste, Startmenü, eigenen Anwendungen, FAT32-Festplatte, Netzwerk und Sound.

Das fertige, bootfähige Image ist **`claudeos.iso`** (BIOS *und* UEFI, auch von USB-Stick bootbar).

## Schnellstart

```sh
make            # baut Kernel, Programme und claudeos.iso
make run        # startet in QEMU (KVM, 1 GB RAM, Festplatte, Netzwerk, Sound)
make run-uefi   # dasselbe mit UEFI-Firmware (OVMF)
make run-tcg    # ohne KVM (langsamer, reine Emulation)
```

`make run` legt beim ersten Start automatisch `build/disk.img` an, eine 512 MB große FAT32-Festplatte
für `/home`. Die Dateien darauf bleiben über Neustarts erhalten und lassen sich auch unter Linux öffnen
(z. B. mit `mdir -i build/disk.img ::`).

Manuell mit QEMU:

```sh
qemu-system-x86_64 -enable-kvm -cpu host -m 1G -vga std \
    -cdrom claudeos.iso -drive file=build/disk.img,format=raw,if=ide \
    -nic user,model=e1000 -audiodev pipewire,id=snd0 -device AC97,audiodev=snd0
```

### VirtualBox

Neue VM vom Typ „Other/Unknown (64-bit)“, mindestens 256 MB RAM (empfohlen: 1 GB):

- **Speicher:** IDE-Controller (PIIX3/PIIX4), ISO als CD, optional eine Festplatte am IDE-Controller
  (kein SATA/AHCI)
- **Grafik:** VBoxVGA oder VMSVGA
- **Audio:** ICH AC97
- **Netzwerk:** NAT, Adaptertyp „Intel PRO/1000 MT Desktop (82540EM)“
- EFI kann an oder aus sein

VirtualBox wurde nicht getestet; alle Tests liefen unter QEMU.

## Bootmenü

| Eintrag | Beschreibung |
|---|---|
| ClaudeOS | Standard, 1024×768 |
| ClaudeOS (1280x720) / (1920x1080) | andere Auflösung beim Start |
| ClaudeOS (800x600, safe mode) | sichere Auflösung |
| ClaudeOS (US keyboard layout) | US- statt deutscher Tastatur |

Die Auflösung lässt sich auch später unter *Settings → Display* ändern.

## Funktionen

**Desktop:** flaches, modernes Design mit Fenstern (Schatten, abgerundete Ecken, verschieben, Größe ändern,
Maximieren, Andocken an den Bildschirmrand), Taskleiste mit Startmenü und Suche, Uhr mit Kalender, Lautstärke-
und Netzwerkanzeige, Benachrichtigungen, Desktop-Symbole, Alt+Tab.

**Anpassbar (Settings):** 7 Themes (hell, dunkel, Retro …), Akzentfarbe, Hintergrundbilder (eigene BMPs
möglich), Taskleistenposition und -größe, Auflösung, Uhrformat, Tastaturlayout (DE/US), Mausgeschwindigkeit,
Lautstärke und Systemklänge. Einstellungen werden in `/home/.config/desktop.cfg` gespeichert.

**Programme:** Terminal, Dateimanager, Texteditor, Taschenrechner, Paint, Bildbetrachter, Musik-Player,
Task-Manager, Einstellungen, Über ClaudeOS – und die Spiele Minesweeper, Snake und Tetris.

**Terminal:** eigene Shell mit Pipes, Umleitungen, `&&`/`||`, Hintergrundprozessen, Verlauf und
Tab-Vervollständigung. Befehle: `ls cat cp mv rm mkdir rmdir touch tree grep wc head tail hexdump
echo env ps kill free df uptime date uname sysinfo dmesg sleep beep open edit ping ifconfig nslookup
wget mkfs reboot poweroff`. `help` zeigt alle Befehle.

**Netzwerk:** Intel-E1000-Treiber, ARP, IPv4, ICMP, UDP, TCP, DHCP und DNS. Zum Ausprobieren:

```sh
ifconfig
ping 10.0.2.2
nslookup example.com
wget http://example.com/
```

**Sound:** AC97-Treiber mit Software-Mixer, `/dev/audio`, Systemklänge und WAV-Wiedergabe im Musik-Player.

**Speicher:** ATA-Festplatten (mit DMA), MBR-Partitionen, FAT32 mit langen Dateinamen, Schreibcache.
Ohne FAT32-Festplatte liegt `/home` im RAM; eine leere Festplatte kann im Terminal mit `mkfs`
formatiert werden.

## Tastenkürzel

| Kürzel | Aktion |
|---|---|
| Windows-Taste / Strg+Esc | Startmenü |
| Strg+Alt+T | Terminal |
| Win+E | Dateimanager |
| Win+D | alle Fenster minimieren |
| Win+←/→/↑/↓ | Fenster links/rechts andocken, maximieren, minimieren |
| Alt+Tab | Fenster wechseln |
| Alt+F4 | Fenster schließen |
| Strg+Alt+Entf / Strg+Umschalt+Esc | Task-Manager |
| Druck | Screenshot als `/home/Screenshot-N.bmp` |

## Technik

- **Boot:** GRUB 2 (Multiboot2), 32-Bit-Einstieg → Long Mode, Higher-Half-Kernel
- **Speicher:** Bitmap-Seitenverwaltung, 4-Level-Paging, NX, Kernel-Heap mit Slabs + vmalloc,
  Prozess-Stacks, die bei Bedarf wachsen
- **Prozesse:** präemptiver Scheduler, Kernel-Threads, Benutzerprozesse in Ring 3, ELF-Loader,
  Systemaufrufe über `int 0x80`, Wait-Queues, Mutexe
- **Dateisysteme:** VFS mit ramfs (Initrd), devfs, Pipes, PTYs und FAT32
- **Treiber:** Framebuffer (VBE/Bochs), PS/2-Tastatur und -Maus, VMware-Absolutmaus, PIT, RTC, serielle
  Schnittstelle, PCI, ATA, AC97, E1000, ACPI (Herunterfahren und Neustart)
- **Grafik:** Fenster-Server im Kernel mit Compositing; Userspace-Bibliothek `libgui` mit Widgets, Menüs
  und Dialogen; eigene C-Bibliothek

```
kernel/   arch (Boot, GDT/IDT, Interrupts) · mm · core (Scheduler, Prozesse, Syscalls) · fs · drv · snd · net · wm
common/   gemeinsamer Code von Kernel und Userspace (ABI, Grafik, Schriften, Bilder)
user/     libc · libgui · apps (GUI-Programme) · bin (Terminal-Befehle)
rootfs/   Dateien der Initrd (/etc, Beispieldateien in /home)
tools/    Asset-Generatoren (Schriften, Icons, Bilder, Klänge), QEMU-Testskript, EFI-Fix für GRUB
```

## Voraussetzungen zum Bauen

gcc, binutils, nasm, make, grub (mit `i386-pc` und `x86_64-efi`), xorriso, mtools, dosfstools und
Python 3 mit Pillow sowie DejaVu-Schriften (für die Assets). Zum Testen QEMU, für UEFI zusätzlich OVMF (edk2).

## Einschränkungen

- nur PS/2-Tastatur/-Maus und IDE-Festplatten (kein USB, SATA/AHCI oder NVMe); auf echter Hardware
  funktioniert das nur, wenn die Firmware PS/2 bzw. IDE emuliert
- nur ein CPU-Kern, keine Hardwarebeschleunigung für Grafik
- nur HTTP, kein HTTPS/TLS; TCP nur als Client
- ein Benutzer, keine Rechteverwaltung
