VISION - automatic Windows Server installation kit
===================================================

WARNING: installing Windows Server ERASES the disk you choose: all files,
programs and Windows 10. Copy everything you want to keep first.

What you need
  - The Windows Server 2022 evaluation ISO (free for 180 days):
    https://www.microsoft.com/evalcenter
  - Rufus: https://rufus.ie
  - A USB stick of 8 GB or more.

Steps
  1. Open autounattend.xml in Notepad and change the password
     Change.Me.2026!  (it appears twice, change both the same way). Save.
  2. Open Rufus, choose the USB stick and the Windows Server ISO, click Start.
     If Rufus asks about Windows customisation, untick everything.
  3. When Rufus says Ready, copy onto the USB stick (the top folder, next to
     setup.exe):
        - autounattend.xml
        - the VISION folder (it contains VISION-Setup.exe)
  4. Plug the USB stick into the PC, restart, and start from the USB stick
     (press F12, F11, F8 or Esc while the PC starts, depending on the brand).
  5. Windows Setup shows ONE screen: the list of disks. Choose the main disk
     (delete its partitions if needed) and click Next. Everything else is
     automatic. The PC restarts a few times.
  6. Windows signs in as Administrator by itself, installs VISION and opens it.
     Give each workplace its keyboard and mouse, Save setup, Start workplaces.
     The first Start offers the free Interception input driver; say Yes and
     restart once.

After installing
  - Change the Administrator password: Ctrl+Alt+Del, Change a password.
  - Install drivers for the graphics card, network and sound if Windows did
    not find them (Device Manager shows a yellow mark).
  - Windows Server allows 2 extra workplaces for free. More need Remote
    Desktop Services licences from Microsoft.
