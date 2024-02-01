CNAME="xla_gpu"
USER="root"
WORK_DIR="/mnt/e/workspace/openxla/"
docker exec -it --workdir=${WORK_DIR} -u ${USER} ${CNAME} /bin/bash
