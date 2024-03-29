# mc

## Dev Environment Setup
* Set up the ESP IDF in a VM (Debian 12 works fine)
* `git clone` this repo
* `idf.py build` **DO NOT** run `idf.py flash`. Software has to be updated only over OTA (see later section)
* The binary built in the above step (`mc/build/mc.bin`) is the one that the OTA process will use
* Every time you make a change to the code, bump up the `version.txt` file. This is strictly speaking not necessary, but will help with catching and debugging OTA issues

## UDP Logging 

After the ESP boots and establishes wifi, logs are copied to a UDP logging server whose IP address and port number are set in the config (see the "Networking Setup" section below).
Logs are also available on the serial port as usual. Early boot up logs, as well as crashes and tracebacks can only be seen on the serial port.
On the logging host, run:

```
socat -u UDP4-RECV:18370,bind=192.168.29.76 -
```

to see the UDP log messages on stdout.

## Networking Setup

Use `idf.py menuconfig` in the project directory to change these values:
```
CONFIG_WLM_WIFI_SSID="swordfish"
CONFIG_WLM_WIFI_PASSWORD=(usual wifi password)
CONFIG_WLM_WIFI_IPV4_ADDRESS="192.168.29.9"
CONFIG_WLM_WIFI_IPV4_MASK="255.255.255.0"
CONFIG_WLM_WIFI_IPV4_GATEWAY="192.168.29.1"
CONFIG_WLM_UDP_LOGGING_IPV4_ADDRESS="192.168.29.76"
CONFIG_WLM_UDP_LOGGING_PORT=18370
```

Since the mc unit has a web server, it's best if it has a static IP address. Likewise, it's best if the UDP Logging server IP address is also static. In order to ensure this, these are configured in the home router's settings:
* https://192.168.29.1 (Jio Centrum Home Gateway)
* Login with admin/usual password
* On left panel, navigate to Network|LAN
* Choose the LAN IPv4 Reserved IPs
* Current settings are:

Computer Name | IP Address | MAC Address
---|---|---
ESP32-mc | 192.168.29.9 | 78:e3:6d:17:e6:38
venkat-dell | 192.168.29.76 | 00:db:df:c7:70:6d

## mc web interface

* `/mc_version_info` (GET method with no arguments)
* `/mc_status` (GET method with no arguments)
* `/mc_ctrl` (POST method)
  - `motor=on`
  - `motor=off`
  - `firmware-upgrade=<url>`
  - `timeofday=<url>`
  
Examples: 
```
curl -d "motor=on" http://192.168.29.9/mc_ctrl
curl -d "motor=off" http://192.168.29.9/mc_ctrl
curl http://192.168.29.9/mc_version_info
curl http://192.168.29.9/mc_status
curl -H "Content-Type: application/x-www-form-urlencoded" -d "firmware-upgrade=https://192.168.29.76:59443/mc.bin" http://192.168.29.9/mc_ctrl
curl -d "timeofday=$(($(date +%s) + 19800))" http://192.168.29.9/mc_ctrl
```

## OTA

OTA works by uploading the latest version of the firmware to a web server, and then invoking the `firmware-upgrade=<url>` http POST command.

OTA uses https, and so TLS has to be set up correctly. We are using Easy-RSA to generate and sign the key and certificates. 
* On deb12-esp there is an Easy-RSA intallation that acts as the CA (CA passkey is managed in revelation)
* On the laptop there is an Easy-RSA installation that generates the server key and certificate, and creates the certificate signing request.
```
cd work/github/easy-rsa/easyrsa3/
./easyrsa gen-req dockerHttpdServer nopass (give the server IP address as the CN)
```
This will create the `.req` file and the `.key` file. The latter is the https server's private key.
* Copy the `.req` file to the CA Easy-RSA installation (i.e. on deb12-esp), and then import and sign the request.
```
cd /home/debian/easy-rsa/easyrsa3
./easyrsa import-req <path to .req file> <some user friendly name>
./easyrsa sign-req server <user friendly name used above>
```
This will generate the `.crt` file.

* There are at this point three files of interest:
 * the `.key` file that is the server's private key
 * the `.crt` file that is the certificate
 * the CA's certificate (stored as `ca.crt` on the CA host, under `/home/debian/easy-rsa/easyrsa3/pki/`)

The first two in that list are to be stored with the https server, and the CA certificate is stored with the https client (under the `server_certs` directory in this repo)

### Setting up and running the Web server
We use the [official httpd docker image](https://hub.docker.com/_/httpd)
 * Copy the original config files from the docker image to local storage:
```
docker run --rm httpd:latest cat /usr/local/apache2/conf/httpd.conf > /home/venkat/work/esp32-water_level_manager/apache-docker/httpd.conf
docker run --rm httpd:latest cat /usr/local/apache2/conf/extra/httpd-ssl.conf > /home/venkat/work/esp32-water_level_manager/apache-docker/httpd-ssl.conf
```
 * Make the following changes to httpd.conf
  * Uncomment the `LoadModule socache_shmcb_module modules/mod_socache_shmcb.so` line
  * Uncomment the `LoadModule ssl_module modules/mod_ssl.so` line
  * Uncomment the `Include conf/extra/httpd-ssl.conf` line

 * Make the following change to httpd-ssl.conf
  * Edit the ServerName directive to `ServerName 192.168.29.76:443`
 
 * Get the following files into a single directory:
  * The server private key file `server.key`
  * The server certificate file `server.crt`
  * The modified `httpd.conf` file
  * The modified `httpd-ssl.conf` file
  
 * Create a Dockerfile with the following content:
```
FROM httpd
COPY ./httpd.conf /usr/local/apache2/conf/httpd.conf
COPY ./httpd-ssl.conf /usr/local/apache2/conf/extra/httpd-ssl.conf
COPY ./server.crt /usr/local/apache2/conf/server.crt
COPY ./server.key /usr/local/apache2/conf/server.key
```
 * Build the docker image:
 ```
 docker build -t apache-docker-esp:2 .
 ```
 * Create a directory that will store the web server content, mainly the `mc.bin` firmware file. It might also be a good idea to store some simple .html files and image files in the content directory.
 
 * Run the docker image:
 ```
 docker run --rm -dit --mount type=bind,src=$(pwd)/content/,dst=/usr/local/apache2/htdocs/ --name httpd-container -p 59443:443 apache-docker-esp:2
 ```
  
