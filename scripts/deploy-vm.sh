#!/usr/bin/env bash
# Deploy do FlowSense na VM. Instalado em /usr/local/bin/flowsense-deploy e chamado
# pelo GitHub Actions por SSH (a chave do deploy só pode rodar este script).
set -euo pipefail

PASTA=/opt/flowsense

exec 9>/tmp/flowsense-deploy.lock
flock -n 9 || { echo "Outro deploy em andamento"; exit 1; }

cd "$PASTA"

echo "==> Atualizando o código"
git fetch --quiet origin main
git reset --quiet --hard origin/main
git submodule sync --quiet
git submodule update --init --recursive --quiet
git log --oneline -1

echo "==> Subindo os containers"
docker compose up -d --build --remove-orphans

echo "==> Conferindo a API"
for i in $(seq 1 30); do
  if docker compose exec -T frontend wget -qO /dev/null http://backend:8080/api/dispositivos 2>/dev/null; then
    docker compose ps
    docker image prune -f >/dev/null
    echo "Deploy concluído"
    exit 0
  fi
  sleep 5
done

echo "A API não respondeu depois do deploy"
docker compose logs --tail 50 backend
exit 1
