#!/usr/bin/env bash
# Simula o ESP32 do FlowSense publicando no broker MQTT, para testar backend e dashboard sem o Wokwi.
# Usa o cliente mosquitto dentro de um container Docker (não precisa instalar nada além do Docker).
#
# Uso:  ./scripts/simular-esp32.sh [dispositivo] [segundos]
#       ./scripts/simular-esp32.sh teste-e2e 60
#
# O dispositivo precisa estar em FLOWSENSE_DISPOSITIVOS no .env do backend.
# Roteiro: online → leituras normais → sobrecorrente (relé desliga) → normal → reinício do ESP32 (kWh volta a 0).
set -euo pipefail

DISPOSITIVO="${1:-teste-e2e}"
DURACAO="${2:-60}"
HOST="${MQTT_HOST:-broker.hivemq.com}"
PORTA="${MQTT_PORT:-1883}"
BASE="flowsense/${DISPOSITIVO}"
IMAGEM="eclipse-mosquitto:2"

publicar() { # publicar <tópico> <mensagem> [-r]
  docker run --rm "$IMAGEM" mosquitto_pub -h "$HOST" -p "$PORTA" -q 1 -t "$1" -m "$2" ${3:-}
}

agora() { date -u +%s; }

echo "Simulando ${DISPOSITIVO} em ${HOST}:${PORTA} por ${DURACAO} s"
publicar "${BASE}/status" "online" -r

# Gera uma leitura a cada 2 s e manda tudo por um único mosquitto_pub (-l = uma mensagem por linha).
gerar_leituras() {
  local kwh=0 total=$((DURACAO / 2)) i tensao corrente potencia rele alerta ts
  for ((i = 1; i <= total; i++)); do
    tensao="221.30"; corrente="5.42"; rele="ON"; alerta="NORMAL"; ts=$(agora)
    # primeiras 3 leituras com timestamp 0 (relógio do ESP32 ainda não sincronizou)
    if ((i <= 3)); then ts=0; fi
    # do meio do teste por 5 leituras: sobrecorrente, relé desligado, potência 0
    if ((i > total / 3 && i <= total / 3 + 5)); then corrente="25.10"; rele="OFF"; alerta="SOBRECORRENTE"; fi
    # nos 2/3 do teste: o ESP32 "reinicia" e o kWh volta a zero
    if ((i == 2 * total / 3)); then kwh=0; fi
    if [[ "$rele" == "ON" ]]; then
      potencia=$(awk -v v="$tensao" -v c="$corrente" 'BEGIN{printf "%.2f", v*c}')
      kwh=$(awk -v k="$kwh" -v p="$potencia" 'BEGIN{printf "%.5f", k + p*2/3600000}')
    else
      potencia="0.00"
    fi
    echo "{\"dispositivo\":\"${DISPOSITIVO}\",\"tensao\":${tensao},\"corrente\":${corrente},\"potencia\":${potencia},\"kwh\":${kwh},\"rele\":\"${rele}\",\"alerta\":\"${alerta}\",\"limites\":{\"tensaoMin\":198,\"tensaoMax\":242,\"correnteMax\":20},\"intervalo\":2000,\"timestamp\":${ts}}"

    # alertas: publicados só quando o estado muda (como o firmware faz)
    if ((i == total / 3 + 1)); then
      publicar "${BASE}/alertas" "{\"dispositivo\":\"${DISPOSITIVO}\",\"tipo\":\"SOBRECORRENTE\",\"tensao\":${tensao},\"corrente\":${corrente},\"rele\":\"OFF\",\"timestamp\":$(agora)}" >&2
    fi
    if ((i == total / 3 + 6)); then
      publicar "${BASE}/alertas" "{\"dispositivo\":\"${DISPOSITIVO}\",\"tipo\":\"NORMAL\",\"tensao\":${tensao},\"corrente\":${corrente},\"rele\":\"ON\",\"timestamp\":$(agora)}" >&2
    fi
    sleep 2
  done
  # uma mensagem inválida no fim: o backend deve registrar em log e descartar
  echo '{"dispositivo":"'"${DISPOSITIVO}"'","tensao":"muito alta","rele":"TALVEZ"}'
}

gerar_leituras | docker run -i --rm "$IMAGEM" mosquitto_pub -h "$HOST" -p "$PORTA" -t "${BASE}/leituras" -l

publicar "${BASE}/status" "offline" -r
echo "Fim da simulação (status offline publicado)."
