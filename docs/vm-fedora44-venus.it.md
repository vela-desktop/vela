# Vela in VM: dall'host a Fedora 44 KDE con Venus

Questa guida prepara un host Linux, crea una VM QEMU/KVM con virt-manager e
installa Vela in Fedora 44 KDE **senza passthrough della GPU**. La GPU rimane
disponibile al desktop dell'host. Il percorso verificato usa un host
Arch/CachyOS con AMD RADV; i comandi di installazione dell'host si riferiscono
a queste distribuzioni. Nel guest si usa Fedora 44 KDE tradizionale con DNF.

Il supporto Vela/Venus descritto qui è **sperimentale**, nel branch
[`feat/vm-venus`](https://github.com/vela-desktop/vela/tree/feat/vm-venus),
[PR 251 in draft](https://github.com/vela-desktop/vela/pull/251). Una normale
release di Vela potrebbe non contenere queste modifiche. La configurazione e
i risultati misurati sono riportati in [virtual-machines.md](virtual-machines.md#verified-guest).

I blocchi marcati **HOST** si eseguono sul computer fisico; quelli marcati
**GUEST** nel terminale di Fedora dentro la VM. Tutti i comandi sono da
eseguire come utente normale, usando `sudo` solo dove indicato.

Percorso: [requisiti](#2-requisiti-e-configurazione-di-riferimento) →
[host](#3-preparare-lhost-archcachyos) →
[creazione VM](#4-creare-e-installare-fedora-44-kde) →
[XML Venus](#5-abilitare-venus-nella-definizione-libvirt) →
[grafica guest](#6-preparare-la-grafica-del-guest) →
[installazione Vela](#7-installare-vela-con-il-supporto-vm) →
[verifica](#8-accedere-a-vela-e-verificare-la-sessione-completa).
Seguono [SSH](#9-ssh-facoltativo-per-la-diagnostica),
[problemi frequenti](#10-problemi-frequenti),
[ripristino](#11-tornare-alla-configurazione-precedente) e
[rebuild di virglrenderer](#appendice-ricompilare-virglrenderer-con-venus).

## 1. Capire quali pezzi servono

| Posizione | Componente | Funzione |
|---|---|---|
| Host | Kernel con KVM e driver GPU | Esegue la VM e gestisce la GPU fisica. |
| Host | Driver Vulkan nativo, per esempio RADV | Esegue sulla GPU le richieste provenienti dal guest. |
| Host | QEMU con backend OpenGL e virglrenderer con Venus | Espone la GPU virtuale e riceve le richieste Vulkan. |
| Host | libvirt e virt-manager | Configurano e avviano la VM. |
| Guest | Kernel `virtio_gpu` e Mesa Venus | Espongono Vulkan attraverso la GPU virtuale. |
| Guest | Mesa Zink | Esegue OpenGL attraverso Vulkan, per i client di Vela. |
| Guest | Vela del branch indicato | Usa l'allocatore Vulkan e seleziona Zink automaticamente su Venus. |

**Non serve installare il driver Mesa Venus sull'host.** L'host usa il driver
della propria GPU; Venus è il driver Vulkan del guest. Su Arch, per esempio,
`vulkan-radeon` è il driver dell'host AMD, mentre `vulkan-virtio` sarebbe il
pacchetto Venus per un guest Arch. In Fedora il driver Venus arriva con
`mesa-vulkan-drivers`.

VirGL e Venus hanno ruoli diversi: abilitare soltanto «accelerazione 3D» in
virt-manager può dare OpenGL al guest senza rendergli disponibile Vulkan.
La distinzione tra i backend è descritta nella
[documentazione QEMU di virtio-gpu](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html).

## 2. Requisiti e configurazione di riferimento

Servono una CPU con virtualizzazione abilitata nel firmware (AMD SVM oppure
Intel VT-x), un driver GPU funzionante e risorse sufficienti anche per il
desktop dell'host. Per questa procedura sono consigliati 8 GiB di RAM, 4–8
vCPU e almeno 40 GiB di disco per la VM, soprattutto se si compila Vela.

Vela richiede **Vulkan 1.4 sul dispositivo del guest**, le estensioni
[DMA-BUF/DRM richieste dal renderer](renderer.md#71-minimum-requirement) e una
CPU x86-64-v3 per i pacchetti normali. Il solo numero «Vulkan Instance Version»
non dimostra che la GPU soddisfi il requisito.

Queste sono le versioni della prova riuscita, non una lista di minimi:

| Componente | Versione/configurazione provata |
|---|---|
| Host | CachyOS, kernel `7.3.3-1-cachyos` |
| GPU host | Radeon RX 9070 XT, RADV/Mesa `26.2.6` |
| Virtualizzazione | QEMU `11.1.3`, libvirt `12.9.0`, virglrenderer `1.3.0` |
| VM | Q35, UEFI, 8 GiB RAM, 8 vCPU, CPU `host-passthrough` |
| Guest | Fedora 44 KDE, kernel `7.3.3-200.fc44.x86_64` |
| Grafica guest | Mesa/Venus `26.2.6`, Vulkan device API `1.4.334` |
| Vela | Modifiche del commit `416bd23`, Qt `6.11.2`, wlroots `0.20.2` |

Per la parte Venus generica, QEMU documenta kernel host 6.13+, virglrenderer
1.0+ e kernel guest 5.16+/Mesa 24.2+. **Questi minimi non garantiscono Vulkan
1.4 e tutte le funzionalità necessarie a Vela**: usare uno stack aggiornato e
completare i controlli nel guest.
[Riferimento QEMU](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html#virtio-gpu-virglrenderer).

Per Intel Meteor Lake/driver `xe`, o GPU discrete abbinate a CPU Intel, Mesa
segnala ulteriori requisiti: kernel 6.16+, QEMU 11+ e l'opzione KVM
`honor-guest-pat=on`. Questo caso non è stato validato con la ricetta libvirt
qui sotto. Consultare le
[note Venus per il driver host](https://docs.mesa3d.org/drivers/venus.html#requirements)
e il supporto della propria versione di libvirt prima di procedere; non
aggiungere una seconda opzione `-accel` alla cieca all'XML.

## 3. Preparare l'host Arch/CachyOS

### Installare QEMU, libvirt e gli strumenti

**HOST:**

```sh
sudo pacman -Syu --needed qemu-desktop libvirt virt-manager virt-viewer \
    edk2-ovmf dnsmasq nftables virglrenderer mesa vulkan-tools
```

Installare anche il driver Vulkan adatto alla GPU scelta. Su AMD con RADV:

```sh
sudo pacman -S --needed vulkan-radeon
```

Su Intel con ANV, il pacchetto corrispondente è `vulkan-intel`. Su NVIDIA
serve uno stack driver compatibile con la GPU, il kernel e Venus: questa
guida non ne sostituisce la procedura di installazione. Non installare un
driver AMD su una GPU Intel o NVIDIA. Dopo un aggiornamento del kernel o del
driver, riavviare l'host prima di continuare.

Il metapacchetto
[`qemu-desktop`](https://archlinux.org/packages/extra/x86_64/qemu-desktop/)
fornisce lo stack desktop di QEMU. Se si usa un'installazione QEMU ridotta,
controllare in particolare i moduli `qemu-ui-opengl` e
`qemu-hw-display-virtio-gpu-gl`.

### Verificare KVM, Vulkan e il backend QEMU

**HOST:**

```sh
uname -r
lscpu
ls -l /dev/kvm /dev/dri/renderD*
vulkaninfo --summary
qemu-system-x86_64 --version
qemu-system-x86_64 -display help
qemu-system-x86_64 -device virtio-vga-gl,help
pacman -Q qemu-desktop libvirt virglrenderer mesa
```

Controllare che:

- `/dev/kvm` esista; se manca, verificare SVM/VT-x nel firmware e il modulo
  `kvm_amd` o `kvm_intel` del kernel;
- `vulkaninfo` elenchi la GPU fisica e il suo driver nativo, con **device API
  1.4**; sull'host non ci si aspetta il nome Venus;
- QEMU offra il display `egl-headless`, il dispositivo `virtio-vga-gl` e le
  proprietà `venus`, `blob` e `hostmem`.

Se il dispositivo GL o una proprietà manca, aggiornare/installare i pacchetti
QEMU appropriati. La presenza della proprietà `venus` prova il supporto lato
QEMU, **non** che la libreria virglrenderer installata sia stata compilata con
Venus. Per quest'ultima verificare la ricetta del pacchetto; la conferma
completa sarà `vulkaninfo` nel guest. Se il pacchetto disabilita Venus, seguire
l'[appendice sulla ricompilazione](#appendice-ricompilare-virglrenderer-con-venus).

Su un host con più GPU, identificare il render node della GPU scelta:

```sh
ls -l /dev/dri/by-path/
udevadm info --query=property --name=/dev/dri/renderD128
```

Usare quel nodo nell'XML più avanti. `renderD128` è un esempio: la numerazione
può cambiare e può indicare una GPU diversa su un altro host.

### Avviare libvirt e la rete NAT

**HOST:** controllare prima il modello di servizi già in uso:

```sh
systemctl is-active libvirtd.service libvirtd.socket \
    virtqemud.service virtqemud.socket
```

Se libvirt funziona già, mantenere il modello esistente. Per una nuova
installazione Arch/CachyOS con demone monolitico:

```sh
sudo systemctl enable --now libvirtd.service
```

Su un sistema predisposto per i demoni modulari, usare invece i socket dei
driver necessari, senza attivare anche `libvirtd`:

```sh
sudo systemctl enable --now virtqemud.socket virtnetworkd.socket \
    virtstoraged.socket virtnodedevd.socket virtnwfilterd.socket \
    virtsecretd.socket virtlogd.socket virtlockd.socket
```

I due modelli e la loro identificazione sono descritti nella
[documentazione dei demoni libvirt](https://libvirt.org/daemons.html).

```sh
sudo virt-host-validate qemu
sudo virsh -c qemu:///system list --all
sudo virsh -c qemu:///system net-list --all
```

Le verifiche relative a KVM devono riuscire. Un avviso su IOMMU, da solo,
non impedisce questo percorso con GPU virtuale.

Se la rete `default` è presente ma inattiva, avviarla e abilitarne l'avvio
automatico:

```sh
sudo virsh -c qemu:///system net-start default
sudo virsh -c qemu:///system net-autostart default
```

Se è già attiva, basta `net-autostart`. Se manca del tutto, crearla da
virt-manager, dettagli della connessione → reti virtuali: nome `default`,
modalità NAT, DHCP abilitato e una sottorete libera. Non creare un bridge
sull'interfaccia fisica per questa procedura.

## 4. Creare e installare Fedora 44 KDE

**HOST, virt-manager:**

1. Scaricare la ISO **Fedora KDE Plasma Desktop 44 x86_64** da una
   [fonte Fedora ufficiale](https://fedoraproject.org/kde/download/), verificando
   il checksum come indicato da Fedora. Usare l'edizione tradizionale con DNF.
2. Aprire virt-manager e connettersi a **QEMU/KVM di sistema**
   (`qemu:///system`). Una connessione utente `qemu:///session` ha gestione
   diversa di rete, dispositivi e permessi e non è quella usata qui.
3. Creare una VM da ISO, selezionare Fedora e assegnare nome
   `fedora44-venus`, 8 GiB RAM, 4–8 vCPU e un disco qcow2 da almeno 40 GiB.
4. Selezionare «personalizza prima dell'installazione». Usare macchina Q35,
   firmware UEFI/OVMF, disco e scheda di rete Virtio, rete NAT `default`.
   Esporre la CPU host, che nell'XML deve risultare `host-passthrough`.
5. Per installare Fedora può bastare video Virtio 2D con display SPICE.
   La configurazione Venus viene applicata nel passo successivo. Non
   aggiungere una periferica PCI della GPU fisica.
6. Aggiungere, se assente, il canale virtio del QEMU guest agent con nome
   `org.qemu.guest_agent.0`. È utile per diagnostica e indirizzo IP, non è
   necessario al rendering di Vela.
7. Installare Fedora sul disco, creare un utente amministratore e accedere
   a KDE. Espellere la ISO dopo l'installazione.

**GUEST:** aggiornare il sistema, quindi spegnerlo completamente:

```sh
sudo dnf upgrade --refresh
sudo systemctl poweroff
```

## 5. Abilitare Venus nella definizione libvirt

Questa configurazione usa **SPICE per la console** e **EGL-headless per il
contesto grafico dell'host**. È quella verificata con libvirt 12.9/QEMU 11.1.
Il supporto GL nativo di SPICE richiede una connessione locale UNIX; tenere
SPICE senza GL e affiancargli EGL-headless permette di mantenere la console
SPICE usata qui.
[Riferimento libvirt per i display](https://libvirt.org/formatdomain.html#graphical-framebuffers).

### Salvare l'XML originale

**HOST:** verificare che la VM risulti spenta, poi salvare una copia:

```sh
VELA_VM_NAME=fedora44-venus
VELA_VM_CONFIG="$HOME/vela-vm-config"
mkdir -p "$VELA_VM_CONFIG"
sudo virsh -c qemu:///system domstate "$VELA_VM_NAME"
(set -C
sudo virsh -c qemu:///system dumpxml --inactive "$VELA_VM_NAME" \
    > "$VELA_VM_CONFIG/${VELA_VM_NAME}.before-venus.xml")
cp -n "$VELA_VM_CONFIG/${VELA_VM_NAME}.before-venus.xml" \
    "$VELA_VM_CONFIG/${VELA_VM_NAME}.venus.xml"
nano "$VELA_VM_CONFIG/${VELA_VM_NAME}.venus.xml"
```

Conservare il primo file come backup e non sovrascriverlo nelle prove
successive: `set -C` impedisce di sovrascriverlo se esiste già. Le modifiche
seguenti sono **frammenti da inserire nell'XML
esistente**, non una definizione completa da sostituire a quella della VM.
Mantenere UUID, firmware, dischi, rete, controller e indirizzi PCI esistenti.

### Namespace, memoria condivisa e CPU

Alla radice `<domain>`, aggiungere il namespace `qemu`, conservando gli
eventuali attributi già presenti:

```xml
<domain type='kvm' xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'>
```

Come figlio diretto di `<domain>`, inserire o modificare l'unico blocco
`memoryBacking`:

```xml
<memoryBacking>
  <source type='memfd'/>
  <access mode='shared'/>
</memoryBacking>
```

Modificare l'eventuale blocco `<cpu>` esistente:

```xml
<cpu mode='host-passthrough' check='none' migratable='off'/>
```

`host-passthrough` qui riguarda la **CPU**, non la GPU. Non può aggiungere
istruzioni x86-64-v3 che la CPU fisica non possiede.

### Display e video

Dentro `<devices>`, mantenere un solo display SPICE, **senza** un suo
`<gl enable='yes'/>`, e aggiungere il display EGL-headless. Per una console
locale, un esempio SPICE con ascolto limitato a localhost è:

```xml
<graphics type='spice' autoport='yes'>
  <listen type='address' address='127.0.0.1'/>
  <image compression='off'/>
</graphics>
<graphics type='egl-headless'>
  <gl rendernode='/dev/dri/renderD128'/>
</graphics>
```

Sostituire `renderD128` se la GPU host selezionata usa un altro render node.
Modificare l'unico dispositivo `<video>` in questo modo, **conservando
l'eventuale `<address>` già assegnato**:

```xml
<video>
  <model type='virtio' device='virtio-vga-gl' heads='1' primary='yes' blob='on'>
    <acceleration accel3d='yes'/>
  </model>
  <alias name='ua-vela-video'/>
</video>
```

L'attributo `device` è accettato dalla versione libvirt provata. Se uno
schema più vecchio lo rifiuta, aggiornare libvirt oppure omettere soltanto
`device='virtio-vga-gl'` e verificare nel comando QEMU generato che
`accel3d='yes'` selezioni comunque `virtio-vga-gl` o `virtio-gpu-gl-pci`.
Il backend 2D non è sufficiente.

### Proprietà Venus e hostmem

Come figlio diretto di `<domain>`, **fuori da `<devices>`**, aggiungere:

```xml
<qemu:override>
  <qemu:device alias='ua-vela-video'>
    <qemu:frontend>
      <qemu:property name='venus' type='bool' value='true'/>
      <qemu:property name='hostmem' type='unsigned' value='4294967296'/>
    </qemu:frontend>
  </qemu:device>
</qemu:override>
```

Se esiste già `qemu:override`, aggiungere il dispositivo al blocco esistente.
L'alias deve essere identico a quello del video. `hostmem` qui vale 4 GiB:
è la finestra di memoria della GPU virtuale, non la RAM totale del guest e
non un'assegnazione esclusiva di 4 GiB di VRAM.
Gli override sono descritti dalla
[documentazione libvirt](https://libvirt.org/drvqemu.html#overriding-properties-of-qemu-devices).

### Validare e applicare

**HOST, nello stesso terminale dove sono definite le variabili:**

```sh
virt-xml-validate "$VELA_VM_CONFIG/${VELA_VM_NAME}.venus.xml" domain
sudo virsh -c qemu:///system domxml-to-native qemu-argv \
    --xml "$VELA_VM_CONFIG/${VELA_VM_NAME}.venus.xml" \
    > "$VELA_VM_CONFIG/${VELA_VM_NAME}.qemu-argv.txt"
cat "$VELA_VM_CONFIG/${VELA_VM_NAME}.qemu-argv.txt"
```

Nel comando generato devono comparire, anche in forma JSON:

```text
memory-backend-memfd ... share=true
-display egl-headless,rendernode=/dev/dri/renderD128
virtio-vga-gl ... blob=true ... venus=true ... hostmem=4294967296
```

Non eseguire il comando QEMU stampato: serve a controllare la traduzione
dell'XML. Se validazione e controllo riescono, definire e avviare la VM:

```sh
sudo virsh -c qemu:///system define \
    "$VELA_VM_CONFIG/${VELA_VM_NAME}.venus.xml"
sudo virsh -c qemu:///system start "$VELA_VM_NAME"
```

Aprire la console in virt-manager. Una modifica al dispositivo video richiede
**spegnimento completo e successivo avvio** della VM: il solo riavvio del
sistema operativo guest mantiene il processo QEMU e non applica queste
modifiche.

## 6. Preparare la grafica del guest

**GUEST, da KDE:**

```sh
sudo dnf install mesa-vulkan-drivers mesa-dri-drivers vulkan-tools pciutils \
    qemu-guest-agent spice-vdagent
sudo systemctl start qemu-guest-agent
uname -r
ls -l /dev/dri/
lspci -k
vulkaninfo --summary
```

Il guest agent richiede il canale configurato al passo 4; se non è presente,
il servizio può non partire. Non blocca la verifica Vulkan.

Il risultato atteso di `vulkaninfo` contiene un dispositivo simile a:

```text
deviceName = Virtio-GPU Venus (AMD Radeon RX 9070 XT ...)
driverID   = DRIVER_ID_MESA_VENUS
apiVersion = 1.4.xxx
```

Il nome della GPU fisica dipende dall'host. Controllare **l'apiVersion di quel
dispositivo**. La presenza aggiuntiva di llvmpipe non è un problema; avere
soltanto llvmpipe/lavapipe non soddisfa questa procedura. Devono esserci anche
un nodo `card*`, un `renderD*` e il driver kernel `virtio_gpu`.

Salvare il rapporto completo per controllare le estensioni richieste da Vela:

```sh
mkdir -p "$HOME/vela-vm-test"
vulkaninfo > "$HOME/vela-vm-test/vulkaninfo.txt" 2>&1
rpm -q mesa-vulkan-drivers mesa-dri-drivers vulkan-tools
vkcube --wsi wayland --c 120
```

Il cubo deve essere visibile e completare i frame. Se KDE è in una sessione
X11, scegliere «Plasma (Wayland)» al login prima di questa prova. Prima di
installare Vela, risolvere un eventuale Vulkan mancante seguendo la
[diagnostica](#10-problemi-frequenti).

## 7. Installare Vela con il supporto VM

**GUEST:** questa procedura compila il branch e installa un RPM nativo con
l'updater del progetto. Non richiede un account GitHub né il download di un
vecchio artifact CI che può essere scaduto.

```sh
sudo dnf install git curl rpm-build dnf5-plugins
mkdir -p "$HOME/src"
cd "$HOME/src"
git clone --branch feat/vm-venus https://github.com/vela-desktop/vela.git vela-vm
cd vela-vm
sh packaging/vela-update --channel experimental \
    --source https://github.com/vela-desktop/vela.git \
    --branch feat/vm-venus --build --jobs 4
```

Eseguire l'updater da un terminale interattivo: richiede `sudo` per installare
le dipendenze con `dnf builddep` e poi l'RPM. Non eseguire l'intero script
come root. Con poca memoria ridurre `--jobs` a 2. Al termine:

```sh
rpm -q vela-git
test -f /usr/share/wayland-sessions/vela.desktop
```

Per riprodurre esattamente il codice della prova, usare nell'updater
`--commit 416bd23b17fbe98d707ad1c72fdc112f50851723` **al posto di**
`--branch feat/vm-venus`, mai insieme. Il branch può invece includere
correzioni successive.

`--branch` e `--commit` selezionano il codice **solo per quella esecuzione**.
Finché il supporto VM rimane su questo branch, aggiornare esplicitamente con:

```sh
vela-update --channel experimental --branch feat/vm-venus --build --jobs 4
```

Un aggiornamento senza selettore torna al branch predefinito del repository.
Anche un rebuild dopo un aggiornamento Qt deve conservare il selettore:

```sh
vela-update --channel experimental --branch feat/vm-venus --force --build --jobs 4
```

La procedura generale, i canali e le politiche dei pacchetti sono documentati
in [installation.md](installation.md#fedora-44-and-45).

### Prova nested facoltativa

Per una prova dentro una finestra KDE, compilare anche un normale albero di
sviluppo seguendo le [dipendenze Fedora](development.md#distribution-dependencies),
poi dal checkout del branch:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel 4
VELA_DEBUG_DMABUF=1 sh scripts/run-nested.sh
```

Lo script cerca i binari in `build/compositor` e `build/shell`; il solo RPM
installato non crea questo albero. Chiudere la finestra o usare
`Alt+Shift+Esc` per terminare la sessione nested. Questa prova verifica il
rendering in finestra, non il percorso DRM/KMS della sessione completa.

## 8. Accedere a Vela e verificare la sessione completa

**GUEST:** uscire da KDE. Nel login manager selezionare la sessione **Vela** e
accedere con lo stesso utente. Conservare KDE come sessione di rientro.

Su Venus il branch seleziona automaticamente:

- l'allocatore **Vulkan DMA-BUF export** per gli output del compositor;
- `MESA_LOADER_DRIVER_OVERRIDE=zink` per i client OpenGL, inclusi shell e Files,
  propagandolo anche all'ambiente di attivazione systemd/D-Bus.

Non occorre impostare `QSG_RHI_BACKEND=vulkan` o forzare globalmente Zink
nell'host o in KDE. Zink è il driver
[OpenGL su Vulkan di Mesa](https://docs.mesa3d.org/drivers/zink.html).
Un override `MESA_LOADER_DRIVER_OVERRIDE` già impostato dall'utente, anche
vuoto, viene rispettato: controllarlo se il comportamento automatico manca.

Dal terminale della sessione Vela:

```sh
printf 'desktop=%s type=%s wayland=%s\n' \
    "$XDG_CURRENT_DESKTOP" "$XDG_SESSION_TYPE" "$WAYLAND_DISPLAY"
loginctl show-session "$XDG_SESSION_ID" -p Type -p Desktop -p Active
pgrep -a -x vela-compositor
pgrep -a -x vela-shell
grep -Ei 'drm|virtio|venus|allocator|zink|Virtual-' \
    "$HOME/.local/state/vela/vela.log"
systemctl --user show-environment | \
    grep -E '^(MESA_LOADER_DRIVER_OVERRIDE|WAYLAND_DISPLAY|XDG_CURRENT_DESKTOP)='
vulkaninfo --summary
vkcube --wsi wayland --c 120
```

Il log deve identificare il backend DRM con il dispositivo `virtio_gpu`,
un output virtuale come `Virtual-1`, il driver Venus e le righe:

```text
Output buffer allocator: Vulkan DMA-BUF export
Venus: OpenGL clients use Zink on Vulkan (MESA_LOADER_DRIVER_OVERRIDE=zink)
```

Se il log mostra invece un backend Wayland annidato, si sta ancora provando
una finestra dentro KDE. La sessione completa verificata usava
`/dev/dri/card1` e `/dev/dri/renderD128`; la numerazione dei nodi del guest
non deve coincidere necessariamente con quella dell'host.

Verificare almeno apertura di Files, spostamento e ridimensionamento delle
finestre, massimizzazione/ripristino, un'applicazione X11 e il cubo Vulkan
visibile. Salvare i log prima di ulteriori esperimenti. Nella prova registrata
funzionavano anche scala 125% e cattura dell'output; lock/unlock, logout,
modalità video e configurazioni multimonitor richiedono ancora verifica.

## 9. SSH facoltativo per la diagnostica

SSH permette di leggere i log anche se la sessione grafica del guest non
parte. Non è necessario esporre la porta 22 del guest verso Internet: con la
rete NAT predefinita l'host raggiunge direttamente l'IP della VM.

**GUEST, nel proprio terminale:**

```sh
sudo dnf install openssh-server
sudo systemctl enable --now sshd
sudo firewall-cmd --permanent --add-service=ssh
sudo firewall-cmd --add-service=ssh
hostname -I
```

**HOST:** sostituire i valori di esempio con utente e IP effettivi del guest:

```sh
sudo pacman -S --needed openssh
VELA_VM_USER=nomeutente
VELA_VM_IP=192.168.122.100
install -d -m 700 "$HOME/.ssh"
ssh-keygen -t ed25519 -f "$HOME/.ssh/vela-vm" -C vela-vm
ssh-copy-id -i "$HOME/.ssh/vela-vm.pub" "$VELA_VM_USER@$VELA_VM_IP"
ssh -i "$HOME/.ssh/vela-vm" "$VELA_VM_USER@$VELA_VM_IP"
```

Scegliere un nome chiave non già utilizzato e verificare l'impronta host al
primo accesso. La chiave privata rimane sull'host. Per cercare l'IP via
libvirt, con il nome della VM impostato nel terminale host:

```sh
sudo virsh -c qemu:///system domifaddr "$VELA_VM_NAME" --source lease
```

Il guest agent di Fedora può leggere informazioni sulla VM ma SELinux può
impedirgli l'esecuzione di comandi amministrativi. Usare SSH o il terminale
del guest per installazioni e configurazioni, mantenendo SELinux abilitato.

## 10. Problemi frequenti

| Sintomo | Controllo e azione |
|---|---|
| QEMU non riconosce `virtio-vga-gl` o non offre `egl-headless` | Installare il backend GL/moduli desktop di QEMU sull'host. |
| QEMU non riconosce `venus`, `blob` o `hostmem` | Aggiornare QEMU; verificare `-device virtio-vga-gl,help`. |
| Il guest ha OpenGL VirGL ma non Venus | Controllare il comando QEMU generato, `venus=true`, `blob=true`, `hostmem`, RAM memfd condivisa e supporto Venus nella build di virglrenderer. La sola spunta 3D non basta. |
| Nel guest compare soltanto llvmpipe/lavapipe | Installare Mesa Vulkan nel guest, verificare il driver kernel `virtio_gpu` e ricontrollare tutta la configurazione host. Non proseguire con Vela finché manca il dispositivo Venus. |
| Venus compare ma offre device API inferiore a 1.4 o mancano estensioni | Aggiornare lo stack host/guest. Una versione alta del loader Vulkan non compensa le capacità del dispositivo. |
| SPICE rifiuta GL con ascolto TCP | Usare SPICE senza GL più EGL-headless come sopra, oppure una configurazione SPICE GL con socket UNIX supportata da libvirt. |
| libvirt rifiuta `device` nel modello video | Aggiornare lo schema oppure omettere quell'attributo e controllare che il dispositivo QEMU risultante sia comunque GL. |
| QEMU non riesce ad aprire il render node | Verificare GPU scelta, esistenza del nodo, permessi del processo QEMU e log libvirt/SELinux. Usare il nodo nell'elemento EGL-headless; evitare `chmod 666`, QEMU come root o la disattivazione delle protezioni di sistema. |
| Il video non cambia dopo un reboot di Fedora | Spegnere la VM, attendere che risulti spenta in libvirt e avviarla di nuovo. |
| Vela fallisce il controllo CPU x86-64-v3 | Esporre la CPU host; verificare che anche la CPU fisica abbia le istruzioni richieste. |
| Vela si chiude all'arrivo della shell con errore Venus `invalid res_id` / `vn_GetMemoryFdPropertiesKHR` | Verificare il branch aggiornato, l'allocatore Vulkan e Zink automatico. Nelle prove l'allocatore da solo non risolveva l'importazione dei buffer EGL/VirGL dei client. |
| Zink automatico non compare | Cercare un override `MESA_LOADER_DRIVER_OVERRIDE` preesistente nei file di ambiente/profilo; Vela rispetta anche un valore esplicitamente vuoto. Controllare pure di non aver forzato `VELA_ALLOCATOR=gbm`. |
| Dopo un aggiornamento Qt la shell non parte | Ricompilare Vela contro il Qt del guest, mantenendo `--branch feat/vm-venus` nel comando di aggiornamento. |
| Compare un rettangolo nero con KDE installato | Controllare `xwaylandvideobridge`: nel guest provato la sua finestra di autostart era visibile sotto le altre finestre. Vedere sotto. |

Per l'ultimo caso, **GUEST, nella sessione interessata**:

```sh
systemctl --user status app-org.kde.xwaylandvideobridge@autostart.service
systemctl --user stop app-org.kde.xwaylandvideobridge@autostart.service
```

Questo arresta il servizio per la sessione corrente; non ne modifica
l'autostart per gli accessi successivi. Non mascherarlo globalmente come
parte dell'installazione: può servire quando si torna a KDE.

Per approfondire un errore del compositor, preservare il log corrente e
consultare [le opzioni di diagnostica](configuration.md), in particolare
`VELA_DEBUG_DMABUF=1`. Non forzare il driver Venus sull'host con
`VK_DRIVER_FILES`: il processo QEMU/virglrenderer deve usare il driver della
GPU fisica.

**HOST**, per errori di avvio della VM:

```sh
sudo journalctl -b -u libvirtd -u virtqemud --no-pager -n 150
sudo tail -n 150 "/var/log/libvirt/qemu/${VELA_VM_NAME}.log"
```

**GUEST**, per raccogliere un rapporto minimo:

```sh
uname -a
rpm -q vela-git mesa-vulkan-drivers mesa-dri-drivers qt6-qtbase wlroots
vulkaninfo --summary
tail -n 200 "$HOME/.local/state/vela/vela.log"
```

Nel rapporto includere anche le versioni host di kernel, QEMU, libvirt,
virglrenderer e driver GPU, oltre al commit Vela e al fatto che la prova sia
nested oppure DRM. I risultati e le osservazioni ancora aperte sono in
[virtual-machines.md](virtual-machines.md#verified-guest).

## 11. Tornare alla configurazione precedente

Se Vela non avvia la sessione, tornare al login manager e scegliere KDE.
Per ripristinare il video della VM, spegnere Fedora completamente e usare
il backup XML dall'host:

```sh
VELA_VM_NAME=fedora44-venus
VELA_VM_CONFIG="$HOME/vela-vm-config"
sudo virsh -c qemu:///system domstate "$VELA_VM_NAME"
sudo virsh -c qemu:///system define \
    "$VELA_VM_CONFIG/${VELA_VM_NAME}.before-venus.xml"
sudo virsh -c qemu:///system start "$VELA_VM_NAME"
```

Procedere con `define` soltanto quando la VM risulta spenta. Il ripristino
della definizione non cancella il disco Fedora. Se necessario, dal guest
in KDE si può rimuovere il solo pacchetto Vela:

```sh
sudo dnf remove vela-git
```

Controllare l'elenco delle rimozioni proposto da DNF prima di confermare.

## Appendice: ricompilare virglrenderer con Venus

Questo passaggio serve **solo se il pacchetto dell'host non abilita Venus**.
La prova registrata usa il pacchetto virglrenderer dell'host; la procedura di
rebuild qui sotto è una possibilità di preparazione, non un secondo stack
già validato in VM. Prima preferire un pacchetto aggiornato della propria
distribuzione.

La build upstream corrente abilita Venus con l'opzione Meson
`-Dvenus=true`, come documentato da
[Mesa](https://docs.mesa3d.org/drivers/venus.html#vtest). Ricette molto vecchie
possono usare nomi diversi: controllare le opzioni della versione realmente
compilata. Il supporto VirGL/OpenGL deve rimanere abilitato per il dispositivo
QEMU GL di questa guida.

Su Arch si può ricostruire il pacchetto nativo, così libvirt carica la
libreria dal percorso di sistema previsto. **HOST:**

```sh
sudo pacman -S --needed base-devel devtools git vulkan-headers
mkdir -p "$HOME/src/vela-vm-host"
cd "$HOME/src/vela-vm-host"
pkgctl repo clone --protocol https virglrenderer
cd virglrenderer
nano PKGBUILD
```

La sintassi per recuperare la ricetta è documentata in
[`pkgctl repo clone`](https://man.archlinux.org/man/pkgctl-repo-clone.1).
Nel PKGBUILD:

1. Leggere la ricetta e verificare la versione dei sorgenti.
2. Nella chiamata Meson/`arch-meson`, o nella lista di opzioni che questa
   usa, aggiungere `-Dvenus=true`; se esiste `-Dvenus=false`, sostituirlo.
3. Incrementare `pkgrel` rispetto alla ricetta originale per distinguere il
   pacchetto locale. Conservare le altre opzioni e dipendenze; se la ricetta
   non prevede le dipendenze Vulkan richieste dalla versione scelta,
   aggiungerle prima della build.

Compilare come utente normale; `makepkg` userà sudo per le dipendenze e
l'installazione:

```sh
makepkg --syncdeps --install
pacman -Q virglrenderer
```

Su CachyOS questa operazione può sostituire il pacchetto ottimizzato della
distribuzione con quello ricostruito dalla ricetta Arch: controllare versione,
dipendenze e transazione prima di accettare. Conservare PKGBUILD modificato e
pacchetto prodotto; un futuro aggiornamento può sostituirlo.

Dopo l'installazione, spegnere completamente e riavviare la VM per caricare la
nuova libreria nel processo QEMU. Ripetere la verifica Venus nel guest. Non
copiare manualmente `.so` in `/usr/lib`, non affidarsi all'`LD_LIBRARY_PATH`
del terminale per un QEMU avviato da libvirt e non installare il driver Mesa
Venus nell'host per tentare di correggere un renderer mancante.
