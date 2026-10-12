TID := 420000000000A1A7

.PHONY: all sysmodule overlay dist clean

all: sysmodule overlay dist

sysmodule:
	@$(MAKE) --no-print-directory -C sysmodule

overlay:
	@$(MAKE) --no-print-directory -C overlay

# SD-Karten-Struktur in out/ zusammenstellen
dist:
	@rm -rf out
	@mkdir -p out/atmosphere/contents/$(TID)/flags out/switch/.overlays
	@cp sysmodule/sys-airplay.nsp out/atmosphere/contents/$(TID)/exefs.nsp
	@cp sysmodule/toolbox.json out/atmosphere/contents/$(TID)/toolbox.json
	@touch out/atmosphere/contents/$(TID)/flags/boot2.flag
	@cp overlay/sys-airplay.ovl out/switch/.overlays/sys-airplay.ovl
	@echo "Fertig: Inhalt von out/ ins Hauptverzeichnis der SD-Karte kopieren."

clean:
	@$(MAKE) --no-print-directory -C sysmodule clean
	@$(MAKE) --no-print-directory -C overlay clean
	@rm -rf out
