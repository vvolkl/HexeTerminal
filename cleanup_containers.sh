nerdctl system prune -f 
nerdctl rmi python:3.9 || true
sudo rm -rf /var/lib/containerd
sudo mkdir /var/lib/containerd
sudo systemctl restart containerd
sudo rm -rf /var/lib/containerd-cvmfs-grpc
sudo mkdir /var/lib/containerd-cvmfs-grpc
sudo systemctl restart cvmfs-snapshotter
