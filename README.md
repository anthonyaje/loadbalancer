## Simple dpdk loadbalancer that reads from 1 phy eth port, creates and writes to two vEth ports    
### BUILD
make

### RUN 
sudo ./build/loadbalancer -c 0x01 -n 4

