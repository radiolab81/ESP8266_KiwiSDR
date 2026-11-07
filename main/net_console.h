/**
 * @file net_console.h
 * @brief Minimale Textkonsole ueber TCP (Ersatz fuer die serielle Eingabe).
 *
 * WARUM?  Beim ESP8266 liegt der I2S-Audioausgang fest auf GPIO3 - das ist zugleich der
 * Empfangspin (RX0) der seriellen Schnittstelle. Solange Audio laeuft, kann der ESP8266
 * ueber die serielle Leitung nichts empfangen. Statt dessen nimmt er Befehle per WLAN an:
 *
 *      nc 192.168.1.50 2323            (oder: telnet 192.168.1.50 2323)
 *      > f 1008        Frequenz in kHz        > m usb   Modus
 *      > s             Status                 > q       Sitzung beenden
 *
 * Es ist keine Anmeldung noetig: jeder im selben Netz kann Frequenz/Modus aendern.
 * Wer das nicht will, schaltet die Konsole in "menuconfig" ab.
 */
#ifndef NET_CONSOLE_H
#define NET_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Wird je Eingabezeile aufgerufen. Antworttext nach @p reply. Rueckgabe wird ignoriert. */
typedef int (*net_console_handler_t)(const char *line, char *reply, size_t reply_cap);

/** Startet den Konsolen-Task (lauscht auf @p port, eine Verbindung gleichzeitig). 0 = ok. */
int net_console_start(uint16_t port, net_console_handler_t handler);

/**
 * Schreibt Text direkt an den verbundenen Client (mehrzeilig moeglich, "\n" -> "\r\n").
 * Fuer Befehle mit langer Ausgabe (Serverliste): der 200-Byte-Antwortpuffer waere zu klein.
 * Ohne Verbindung passiert nichts.
 */
void net_console_write(const char *s);

#ifdef __cplusplus
}
#endif
#endif
