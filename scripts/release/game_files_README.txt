Put the extracted contents of YOUR OWN SSX 3 Xbox disc here.

This release contains no game data. You need a legally obtained copy of
SSX 3 for the original Xbox, PAL/EU version (title ID 4541003C), which is the
build this port was made from. Other versions (NTSC-U, NTSC-J) do not work.

After extracting the disc image this folder must look like this:

  game_files/
    default.xbe
    data/
    ...

To extract an .iso image use extract-xiso (https://github.com/XboxDev/extract-xiso)
or xdvdfs (https://github.com/antangelo/xdvdfs):

  extract-xiso -x "SSX 3.iso" -d game_files
  xdvdfs unpack "SSX 3.iso" game_files
