# blkout

### Über:

Einige, gerade ältere Grafikkarten haben unter Wayland Probleme beim Wiederaktivieren des Bildschirms nach dem Standby. Bei mir traten die Probleme nach der Zwangsbeglückung mit Wayland bei meinem ThinkCentre M920q mit Intel® UHD Graphics 630 auf. Nach dem Aktivieren der Standbyfunktion war ein Aufwecken des Bildschirms anschließend nicht mehr möglich. Kurioserweise trat dieses Problem nicht beim Ruhezustand auf, nur beim Standby. Der Ruhezustand war für mich keine Option, da der Bildschirm zwar schwarz sein, der Computer aber weiter arbeiten sollte. Hier kommt blkout als Workaround ins Spiel.

blkout zeigt ein schwarzes Vollbild-Overlay über allen Fenstern an und wird bei Tastendruck oder Mausbewegung wieder geschlossen. Das ist zwar nicht so energiesparend wie echtes Standby, erfüllt aber seinen Zweck.

Getestet wurde blkout unter Manjaro Anh-Linh KDE/Plasma 6.5.5 (Wayland).

### Bedienung:

Der Aufruf erfolgt mit `blkout -s <sekunden>`. Der Bildschirm wird nach der angegebenen Anzahl von Sekunden schwarz geschaltet. Eine Tastatureingabe oder Mausbewegung „weckt" den Bildschirm wieder auf. `blkout -e` beendet das Programm nach der Ausführung.

Unter Plasma bietet es sich an, blkout in der Energieverwaltung unter „Andere Einstellungen" bei „Inaktivität nach n Minuten: Skript ausführen" einzutragen. Der Eintrag sähe dann folgendermaßen aus: `/usr/local/bin/blkout -e`

### Installation:

In das Verzeichnis `blkout/` wechseln und mit `sudo make install` kompilieren. Nach dem Kompilieren findet sich das lediglich 33 KB große Binary unter `/usr/local/bin/blkout`.

---

# blkout

### About:

Some graphics cards, especially older ones, have problems reactivating the display after standby under Wayland. In my case, these problems occurred after being forced to switch to Wayland on my ThinkCentre M920q with Intel® UHD Graphics 630. After enabling the standby function, waking the display was no longer possible. Curiously, this problem did not occur with suspend-to-disk, only with standby. Suspend-to-disk was not an option for me, as I needed the screen to go black while the computer continued working. This is where blkout comes in as a workaround.

blkout displays a black fullscreen overlay on top of all windows and closes it again on any keypress or mouse movement. While not as energy-efficient as true standby, it does the job.

Tested on Manjaro Anh-Linh KDE/Plasma 6.5.5 (Wayland).

### Usage:

Invoke with `blkout -s <seconds>`. The screen will go black after the specified number of seconds. A keypress or mouse movement wakes it again. `blkout -e` exits the program after the overlay is dismissed.

Under Plasma, it is convenient to add blkout to the power management settings under "Other Settings" at "Run script on idleness after n minutes". The entry would look like this: `/usr/local/bin/blkout -e`

### Installation:

Change into the `blkout/` directory and compile with `sudo make install`. After compilation, the binary – only 33 KB in size – can be found at `/usr/local/bin/blkout`.
