IMAGE="tensorflow/build:latest-python3.9"
CNAME="xla_gpu"
USER="root"
docker run -dit --name=${CNAME} \
--privileged --network=host --cap-add=sys_ptrace \
--shm-size=1G \
-v /mnt/e/workspace:/mnt/e/workspace \
-u ${USER} \
${IMAGE} bash
