# eotcp

`eotcp` is a tunneling client-server application that connects 2 ethernet endpoints throught a TCP connection. Ethernet side implementation relies on linux TAP devices.

The TAP devices behave as if they were connected through a cable:

```
|      eotcp -c      |         |     eotcp -s       |     
|tap0 <-> TCP(client)|<------->|TCP(server) <-> tap0|
```

Usage: `eotcp [-p <port>] [-t <tap> ] { -c <server_addr> | -s <bind_addr> }`

The same binary is used for the client and server part. The mode is selected with `-c` or `-s`.

- The default TCP port for transport is 4242, and can be overridden by `-p`.
- When `-t` is used, eotcp uses the provided interface name. If the interface has been created with an unpriviledged user name, then the whole process can run unpriviledged.
  `ip tuntap add <tap_device_name> mode tap user <user>`
- When `-t` is not used, eotcp creates a TAP interface with the default name (this requires to be root or have NETCAP), typically `tap0`.

# snippets

Run the server part on 10.1.1.1 and bridge on br-lan:

```
eotcp -s 10.1.1.1 &
ip link set tap0 master br-lan up
```

As user `nobody`, connect a qemu VM using tap device `vm` to 10.1.1.1:

```
sudo ip tuntap add vm mode tap user nobody
sudo ip tuntap add eotcp0 tap user nobody
sudo ip link add br-eotcp type bridge
sudo ip link set vm master br-eotcp up
sudo ip link set eotcp master br-eotcp up
sudo ip link set br-eotcp up
eotcp -t eotcp0 -c 10.1.1.1 &
qemu ... -netdev tap,id=ether,ifname=vm,script=,downscript= -device e1000,netdev=ether
```

