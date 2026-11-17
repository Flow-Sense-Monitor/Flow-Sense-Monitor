# FlowSense

Monitoramento de energia elétrica em tempo real com IoT. Um ESP32 (simulado no Wokwi) mede tensão e corrente, calcula potência e consumo, aplica as regras de proteção localmente (desliga a carga por um relé) e publica tudo por MQTT. Um backend Spring Boot guarda os dados em MySQL e expõe uma API REST, e um dashboard React mostra os valores, gráficos, custo estimado e alertas, e permite ligar/desligar a carga e ajustar os limites.

## Arquitetura (4 camadas)

```text
┌──────────────────────┐   ┌──────────────────────┐   ┌───────────────────────────┐   ┌──────────────────────┐
│ 1. PERCEPÇÃO         │   │ 2. REDE              │   │ 3. PROCESSAMENTO          │   │ 4. APLICAÇÃO         │
│ ESP32 (Wokwi)        │   │ Wi-Fi + MQTT         │   │ Spring Boot + MySQL       │   │ Dashboard React      │
│ • potenciômetros =   │──▶│ broker.hivemq.com    │──▶│ • assina os tópicos       │──▶│ • valores ao vivo    │
│   tensão e corrente  │   │ :1883                │   │ • valida e grava          │   │ • gráficos 1h/24h/7d │
│ • regra local: relé, │◀──│ tópicos              │◀──│ • histórico por minuto    │◀──│ • custo, alertas     │
│   LEDs, buzzer, OLED │   │ flowsense/{id}/...   │   │ • API REST + comandos     │   │ • relé e limites     │
└──────────────────────┘   └──────────────────────┘   └───────────────────────────┘   └──────────────────────┘
        firmware                 MQTT (pub/sub)              MQTT ⇄ REST                    somente HTTP REST
```

- O ESP32 e o backend **não se conectam diretamente**: os dois falam com o mesmo broker MQTT.
- O dashboard fala com o backend **somente por HTTP REST** (busca a leitura atual a cada 2 s).
- A proteção é decidida **no ESP32**: funciona mesmo sem internet. O backend só registra e repassa comandos.
  - Valor fora da faixa (sobrecorrente, sobretensão ou subtensão): alerta na hora (LED vermelho, buzzer intermitente, OLED e dashboard), sem desligar a carga.
  - Voltou ao normal em menos de 5 s (pico ou queda rápida): o alerta some e nada é desligado.
  - Ficou 5 s seguidos fora da faixa: o relé desliga e o buzzer toca sem parar. A carga só volta quando alguém clica em **Ligar carga** no dashboard. Se o problema continuar, a proteção desliga de novo depois de 5 s.

## Estrutura

```text
flowsense/
├── wokwi/                # código do ESP32 usado na simulação (sketch.ino + libraries.txt)
├── flowsense-firmware/   # reservado para o hardware físico — submódulo
├── flowsense-backend/    # Spring Boot 4, Java 21, MySQL, Flyway, Paho MQTT — submódulo
├── flowsense-frontend/   # React 18 + Vite + Recharts, servido por nginx — submódulo
├── scripts/simular-esp32.sh  # simula o ESP32 publicando no broker (testes sem Wokwi)
├── docs/seguranca.md     # checklist OWASP IoT Top 10
├── docker-compose.yml    # sobe MySQL + backend + frontend
└── .env.example          # variáveis de ambiente (copie para .env)
```

## Tópicos MQTT e payloads

`{id}` é o identificador do dispositivo (hoje `esp32-01`). Exemplos reais publicados pelo firmware:

| Tópico | Quem publica | Quando | Conteúdo |
|---|---|---|---|
| `flowsense/{id}/leituras` | ESP32 | a cada 2 s (configurável) | JSON com a leitura completa |
| `flowsense/{id}/alertas` | ESP32 | só quando o alerta muda | JSON com o tipo do alerta |
| `flowsense/{id}/status` | ESP32 / broker | ao conectar / quando cai | texto `online` ou `offline` (retido; o `offline` é a "última vontade" publicada pelo broker) |
| `flowsense/{id}/comandos` | backend | quando o usuário age no dashboard | JSON com o comando |

```json
// flowsense/esp32-01/leituras
{"dispositivo":"esp32-01","tensao":221.30,"corrente":5.42,"potencia":1199.45,
 "kwh":0.00213,"rele":"ON","alerta":"NORMAL",
 "limites":{"tensaoMin":198,"tensaoMax":242,"correnteMax":20},
 "intervalo":2000,"timestamp":1794445800}

// flowsense/esp32-01/alertas   (tipo: SOBRETENSAO | SUBTENSAO | SOBRECORRENTE | NORMAL)
{"dispositivo":"esp32-01","tipo":"SUBTENSAO","tensao":192.09,"corrente":19.53,"rele":"OFF","timestamp":1794529491}

// flowsense/esp32-01/comandos
{"rele":"ON"}
{"limites":{"tensaoMin":200,"tensaoMax":240,"correnteMax":15}}
{"intervalo":5000}
```

Regras que o firmware aceita (e que o backend e o dashboard também validam): `0 < tensaoMin < tensaoMax <= 300`, `0 < correnteMax <= 30`, `500 <= intervalo <= 60000` ms.

Detalhes tratados pelo backend:
- `timestamp` vem `0` enquanto o relógio do ESP32 não sincronizou → usa o horário do servidor.
- `kwh` é acumulado desde que o ESP32 ligou e volta a `0` quando ele reinicia → o consumo é a soma das diferenças positivas entre leituras; quando o valor diminui, conta como reinício (nunca gera consumo negativo).
- Com o relé desligado, `corrente` mostra o que a carga pediria, mas `potencia` vem `0` → backend e dashboard usam a potência enviada, sem recalcular.
- O broker é público → o backend só aceita os IDs listados em `FLOWSENSE_DISPOSITIVOS` e valida cada mensagem; mensagens inválidas vão para o log e são descartadas.

## API REST

| Método e rota | Função |
|---|---|
| `GET /api/dispositivos` | dispositivos configurados e se estão online |
| `GET /api/dispositivos/{id}/leitura-atual` | última leitura, relé, alerta, limites, intervalo, online |
| `GET /api/dispositivos/{id}/historico?de=&ate=` | pontos por minuto para os gráficos (agrupa em 5/30 min em períodos longos) |
| `GET /api/dispositivos/{id}/consumo?de=&ate=` | kWh do período e custo estimado |
| `GET /api/dispositivos/{id}/eventos?limite=` | alertas e eventos, mais recentes primeiro (padrão 50, máx. 200) |
| `POST /api/dispositivos/{id}/rele` | `{"estado":"ON"\|"OFF"}` — exige `X-API-Key` |
| `PUT /api/dispositivos/{id}/limites` | `{"tensaoMin":200,"tensaoMax":240,"correnteMax":15}` — exige `X-API-Key` |
| `PUT /api/dispositivos/{id}/intervalo` | `{"intervalo":5000}` — exige `X-API-Key` |

`de` e `ate` em ISO-8601 UTC (ex.: `2026-11-14T13:00:00Z`); sem eles, a última hora. Erros vêm em JSON: `{"status":400,"erro":"Bad Request","mensagem":"...","campos":{...},"momento":"..."}`. Comandos respondem `202 Accepted` (o ESP32 confirma na próxima leitura), `401` sem chave, `400` com valor inválido e `503` se o backend estiver sem conexão com o broker.

Banco (MySQL, tabelas criadas pelo Flyway em `flowsense-backend/src/main/resources/db/migration`):
- `leitura_atual` — uma linha por dispositivo, sobrescrita a cada leitura;
- `historico_resumido` — uma linha por dispositivo e minuto (médias, mínimos/máximos e consumo do minuto);
- `eventos_importantes` — alertas, online/offline e comandos enviados.

Histórico e eventos com mais de `FLOWSENSE_RETENCAO_DIAS` são apagados uma vez por dia.

## Como rodar em outra máquina

Pré-requisitos: [Docker](https://docs.docker.com/get-docker/) com o plugin Compose e Git.

```bash
git clone --recurse-submodules <url-deste-repositorio> flowsense
```

```bash
cd flowsense
```

```bash
cp .env.example .env
```

Edite o `.env`: troque `DB_PASSWORD`, `DB_ROOT_PASSWORD` e `FLOWSENSE_API_KEY` (gere com `openssl rand -hex 24`), e coloque em `FLOWSENSE_CORS_ORIGENS` o endereço pelo qual vai abrir o dashboard. Depois:

```bash
docker compose up -d --build
```

A primeira subida leva alguns minutos (compila backend e frontend). Abra `http://localhost` (ou a porta de `HTTP_PORT`). Para ver os logs:

```bash
docker compose logs -f backend
```

Em seguida, rode a simulação no Wokwi: cole `wokwi/sketch.ino` no sketch do projeto e o conteúdo de `wokwi/libraries.txt` na aba `libraries.txt` (o `DEVICE_ID` do sketch precisa estar em `FLOWSENSE_DISPOSITIVOS`). Sem o Wokwi, dá para simular o ESP32 (adicione `teste-e2e` em `FLOWSENSE_DISPOSITIVOS` e reinicie o backend):

```bash
./scripts/simular-esp32.sh teste-e2e 60
```

### Desenvolvimento sem Docker

- Backend: MySQL 8 local com o banco `flowsense`; exporte `DB_PASSWORD` e `FLOWSENSE_API_KEY` e rode `./mvnw spring-boot:run` em `flowsense-backend/`. Testes: `./mvnw test`.
- Frontend: em `flowsense-frontend/`, copie `.env.example` para `.env` (mesma chave em `VITE_API_KEY`), `npm install` e `npm run dev`. O Vite repassa `/api` para `http://localhost:8080`.

## Variáveis de ambiente (`.env`)

| Variável | Padrão | Para que serve |
|---|---|---|
| `DB_USER` | `flowsense` | usuário do MySQL usado pelo backend |
| `DB_PASSWORD` | — (obrigatória) | senha desse usuário |
| `DB_ROOT_PASSWORD` | — (obrigatória) | senha de root do MySQL (só o container do banco usa) |
| `MQTT_HOST` | `broker.hivemq.com` | broker MQTT (o mesmo do ESP32) |
| `MQTT_PORT` | `1883` | porta do broker (`8883` com TLS) |
| `MQTT_USER` / `MQTT_PASSWORD` | vazio | credenciais, se o broker exigir |
| `MQTT_TLS` | `false` | `true` para conexão criptografada (`ssl://`) |
| `FLOWSENSE_DISPOSITIVOS` | `esp32-01` | IDs aceitos, separados por vírgula |
| `FLOWSENSE_API_KEY` | — (obrigatória) | chave exigida no cabeçalho `X-API-Key` dos comandos; também é embutida no dashboard |
| `FLOWSENSE_TARIFA_KWH` | `0.95` | tarifa em R$/kWh para o custo estimado |
| `FLOWSENSE_CORS_ORIGENS` | `http://localhost` | origens do navegador autorizadas a chamar a API |
| `FLOWSENSE_RETENCAO_DIAS` | `90` | dias de histórico e eventos guardados |
| `HTTP_PORT` | `80` | porta pública do dashboard |
| `BACKEND_PORT` | `8080` | porta do backend, só em `127.0.0.1` (testes com `curl` dentro da VM) |
| `DB_PORT` | `3306` | porta do MySQL, só em `127.0.0.1` (MySQL Workbench via túnel SSH) |

## Implantação na VM Oracle Cloud (Free Tier)

Funciona tanto na VM ARM (Ampere A1) quanto na x86 (`VM.Standard.E2.1.Micro`, 1 GB de RAM): todas as imagens existem para `arm64` e `amd64`, a JVM usa no máximo 70% da memória do container e o MySQL está configurado para pouca memória.

1. **Crie a VM** (Ubuntu 22.04/24.04) e acesse por SSH:
   ```bash
   ssh -i sua-chave.key ubuntu@IP-DA-VM
   ```
2. **Na VM de 1 GB, crie swap** (a compilação do backend precisa de mais memória do que a VM tem):
   ```bash
   sudo fallocate -l 2G /swapfile && sudo chmod 600 /swapfile && sudo mkswap /swapfile && sudo swapon /swapfile && echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab
   ```
3. **Instale o Docker**:
   ```bash
   curl -fsSL https://get.docker.com | sudo sh && sudo usermod -aG docker $USER
   ```
   Saia e entre de novo no SSH para o grupo valer.
4. **Clone e configure** (como em "Como rodar em outra máquina"), com `FLOWSENSE_CORS_ORIGENS=http://IP-DA-VM`, e suba:
   ```bash
   docker compose up -d --build
   ```
5. **Libere a porta 80 na Security List** (é aqui que costuma travar). No console da Oracle: *Networking → Virtual Cloud Networks → sua VCN → Subnet → Security List → Add Ingress Rules*:
   - Source CIDR `0.0.0.0/0`, IP Protocol `TCP`, Destination Port Range `80`.
   - (Não abra 8080 nem 3306: backend e banco ficam acessíveis só dentro da VM.)
6. **Libere a porta 80 no firewall do Ubuntu**. As imagens Ubuntu da Oracle vêm com regras `iptables` que bloqueiam tudo além do SSH, mesmo com a Security List liberada:
   ```bash
   sudo iptables -I INPUT 6 -m state --state NEW -p tcp --dport 80 -j ACCEPT
   ```
   ```bash
   sudo netfilter-persistent save
   ```
   (Em Oracle Linux, use `sudo firewall-cmd --permanent --add-port=80/tcp && sudo firewall-cmd --reload`.)
7. Abra `http://IP-DA-VM`. Para conferir de dentro da VM: `curl http://localhost/api/dispositivos`.

### Se a VM já tiver um nginx usando a porta 80

É o caso da VM do grupo, que também hospeda outro site. Nela, o FlowSense fica atrás do nginx da própria VM:

- no `.env`: `HTTP_PORT=127.0.0.1:8088` (o dashboard só escuta localmente), `BACKEND_PORT=8082` (a 8080 já estava em uso) e `FLOWSENSE_CORS_ORIGENS=http://IP-DA-VM`;
- no site padrão do nginx (`/etc/nginx/sites-available/default`, o que atende quem acessa pelo IP), `location / { proxy_pass http://127.0.0.1:8088; }` com os cabeçalhos `Host` e `X-Forwarded-*`. Depois, `sudo nginx -t && sudo systemctl reload nginx`.

Os outros sites, que têm `server_name` próprio, não são afetados, e nenhuma porta nova precisa ser liberada na Security List. O dashboard fica em `http://IP-DA-VM`.

Os containers têm `restart: unless-stopped`: voltam sozinhos se a VM reiniciar. Se o broker cair, o backend reconecta sozinho.

Para atualizar depois de um `git pull`: `docker compose up -d --build`. Os dados do MySQL ficam no volume `mysql-dados`.

### Ver o banco com o MySQL Workbench

O MySQL escuta só em `127.0.0.1` da VM (porta `DB_PORT`), então o acesso é por túnel SSH. Nenhuma porta nova é aberta na internet.

- **Connection Method:** `Standard TCP/IP over SSH`
- **SSH Hostname:** `IP-DA-VM:22` · **SSH Username:** usuário do SSH · **SSH Key File:** a chave privada da VM
- **MySQL Hostname:** `127.0.0.1` · **MySQL Server Port:** `3306` (o `DB_PORT`)
- **Username:** `flowsense` (o `DB_USER`) · **Password:** o `DB_PASSWORD` do `.env` da VM · **Default Schema:** `flowsense`

As tabelas são criadas pelo Flyway (`flyway_schema_history` registra as migrações). Use o Workbench para consultar: alterar tabelas à mão pode quebrar a validação do Hibernate na próxima subida do backend.

## Decisões de projeto

- **Polling em vez de WebSocket no dashboard**: a proposta define que o frontend fala com o backend somente por REST. A leitura atual é buscada a cada 2 s (o mesmo intervalo do ESP32); histórico a cada 30 s; eventos a cada 5 s.
- **Histórico agregado por minuto** (upsert com médias incrementais) em vez de gravar cada leitura de 2 s: ~1.440 linhas por dia por dispositivo, suficiente para os gráficos e leve para o disco da VM. Gráficos de 24 h e 7 dias agrupam em pontos de 5 e 30 minutos.
- **Consumo de um intervalo sem backend** (ex.: backend reiniciando) é atribuído ao minuto da próxima leitura, porque o kWh é acumulado no ESP32.
- **Dispositivo offline** no dashboard: quando o broker publica `offline` (LWT) ou quando passam mais de 3 intervalos (mínimo 15 s) sem leitura.
- **Eventos de online/offline** só são gravados quando o estado muda (a mensagem `status` é retida e chega de novo a cada reconexão).
- **Comandos** são publicados com QoS 1, sem retenção (um comando antigo não deve ser reexecutado quando o ESP32 reconectar). O backend responde `202`, e o estado real aparece na próxima leitura.
- **nginx do frontend repassa `/api`** ao backend: o navegador acessa um único endereço e as portas do backend e do banco não ficam expostas.
- **Cliente MQTT Eclipse Paho direto** (sem Spring Integration), com ID aleatório, reconexão automática e nova assinatura a cada reconexão.

## Segurança

Veja [docs/seguranca.md](docs/seguranca.md) (checklist OWASP IoT Top 10, com o que foi feito e as limitações conhecidas).
