# Boiler Monitor V2

## Status e seguranca

A V2 esta em desenvolvimento na branch `v2-development`. A V1 permanece
preservada no tag `v1-stable` (commit `b4998d0`) e no branch original `main`.
O firmware V2 ainda precisa de ensaio nas duas placas e no local de instalacao.
Os ambientes `v2-display` e `v2-acquisition` foram compilados com PlatformIO.
Nao houve teste fisico das placas, sensores, rede ou rele; nao conecte o
aquecimento durante os testes iniciais.

O firmware mantem o rele desligado no boot. A perda de telemetria por mais de
2 segundos, uma leitura invalida ou temperatura de falha desliga o rele e
trava a falha; para rearmar, a comunicacao precisa estar valida, pelo menos
cinco amostras precisam ser aceitas e a temperatura deve estar abaixo do limite
de reconhecimento. O comando exibido continua sendo o estado enviado ao rele,
nao uma confirmacao eletrica do contator. Preserve o termostato independente.

## Arquitetura

```text
 Sondas NTC e medidores de fluxo
              |
       cabos analogicos curtos
              v
 +---------------------------+          Wi-Fi 2,4 GHz local
 | ESP32 de aquisicao        |  UDP broadcast, CRC16, sequencia
 | ADC, filtro, NTC, pulsos  | ================================+
 +---------------------------+                                |
                                                               v
                                                    +----------------------+
                                                    | ESP32 com display    |
                                                    | interface, agenda,   |
                                                    | Wi-Fi, NTP e rele    |
                                                    +----------------------+
```

As duas placas entram na mesma rede Wi-Fi 2,4 GHz do roteador. Nao precisam
de internet para trocar telemetria, mas a funcao de horario NTP continua
dependendo de acesso a internet. O roteador deve permitir broadcast local e
nao pode isolar clientes Wi-Fi (rede guest/AP isolation).

| Tecnologia | Adequacao para 10 m junto a potencia | Observacao |
|---|---|---|
| UART TTL | Baixa | Nivel logico sem diferencial; evitar cabo longo nesse ambiente |
| I2C | Muito baixa | Barramento curto entre placas, sensivel a capacitancia/ruido |
| RS-485 | Melhor imunidade | Recomendacao eletrica para a instalacao, mas requer transceptor em cada placa |
| ESP-NOW | Media | Sem roteador, mas continua sendo radio e sujeito a canal/interferencia |
| Wi-Fi local | Media | Escolhido por nao exigir modulos extras e usar a rede existente; depende do roteador/broadcast |

O display continua com o painel web e ponto de acesso `Boiler-Config`. A placa
de aquisicao cria `Boiler-Aquisicao` para provisionamento quando nao tem
credenciais salvas ou nao consegue entrar no roteador. Configure ambas para a
mesma rede. O ponto de acesso de aquisicao usa a senha `AP_PASSWORD` de
`include/config.h`; altere-a antes de operar e nao exponha a interface web na
internet.

## Comunicacao e protocolo

- Transporte: UDP broadcast IPv4 na porta `4210`, a cada 250 ms.
- Formato binario de 44 bytes, campos little-endian compartilhados pelos ESP32.
- Cabecalho: magic `BOL2`, versao, mascara de validade, sequencia e identificador
  de boot da unidade de aquisicao.
- Dados: tres temperaturas em centesimos de grau Celsius, tres leituras ADC em
  mV e contadores cumulativos de pulsos de entrada e saida.
- Integridade: CRC-16/CCITT (polinomio `0x1021`, inicial `0xFFFF`) sobre os
  primeiros 42 bytes.
- O receptor descarta tamanho, magic, versao, CRC e sequencias invalidas/
  repetidas. Contadores cumulativos recuperam pulsos mesmo quando um datagrama
  e perdido.
- Sem pacote valido por 2 segundos: link declarado indisponivel e rele
  desligado. O reconhecimento de falha continua manual.

CRC detecta erros acidentais; nao autentica o remetente nem cifra a telemetria.
Use uma rede Wi-Fi WPA2/WPA3 confiavel e isolada de redes publicas.
Broadcast/UDP depende da configuracao do roteador; validar o comportamento
real no ponto de instalacao, especialmente perto do contator.

## Pinagem e ligacoes

Os pinos abaixo reaproveitam os pinos definidos em `include/config.h` e o
firmware V1. O codigo de aquisicao espera um ESP32 classico com ADC1 e estes
GPIOs expostos. A placa nova foi identificada apenas como “ESP32”; confira o
modelo/revisao e a disponibilidade dos pinos antes de soldar ou energizar.
Nao use estes valores em ESP32-C3/S2/S3 sem revisar o mapa de GPIO/ADC.

### Placa de aquisicao (ESP32 classico, a confirmar fisicamente)

| Sinal | GPIO | Ligacao/configuracao |
|---|---:|---|
| NTC do boiler (B8660) | 32 / ADC1 | Divisor existente: 3V3 - 10 kOhm - ADC - NTC - GND |
| NTC entrada solar (47 kOhm) | 33 / ADC1 | Divisor: 3V3 - 47 kOhm - ADC - NTC - GND |
| NTC saida solar (47 kOhm) | 36 / ADC1 | Mesmo divisor; GPIO36 e somente entrada |
| Pulso medidor entrada | 25 | Entrada digital com interrupcao |
| Pulso medidor saida | 26 | Entrada digital com interrupcao |
| Alimentacao | 3V3/GND | Sensor/divisor com terra local comum |

O firmware presume NTC 10 kOhm/B3950 no boiler, NTC 47 kOhm/B3950 nas linhas
solares, resistores serie indicados acima e referencia de 3,3 V. Esses sao os
parametros ja presentes no codigo, nao uma confirmacao de datasheet ou
calibracao. Verifique a resistencia a temperatura conhecida, curva/Beta e
valores/tolerancia dos resistores antes de habilitar o aquecimento. Os offsets
`*_TEMP_OFFSET_C` estao em zero e sao pontos de calibracao, nao correcao ja
medida.

Nao aplique sinal de 5 V diretamente aos GPIOs de pulso. Se o medidor for
confirmado como push-pull de 5 V, a documentacao existente da V1 recomenda um
divisor por saida (12 kOhm entre sinal e GPIO e 22 kOhm entre GPIO e GND).
Confirme o tipo eletrico do medidor antes de usar esse divisor; se a saida ou
pinagem for desconhecida, nao conecte.

### Placa com display

O rele e a interface existente permanecem nesta placa: GPIO27, ativo em LOW.
Display ST7789: GPIO19 MOSI, GPIO18 SCLK, GPIO5 CS, GPIO16 DC, GPIO23 reset e
GPIO4 backlight. Botoes: GPIO35 (+, pull-up externo na placa) e GPIO0 (-).
Os GPIOs 32/33/36 e 25/26 deixam de ler sensores nesta placa na V2.

Confira a polaridade e o modulo de rele instalados. Mantenha a interface de
rele compativel com logica de 3,3 V e a polarizacao externa que mantem o
aquecimento desligado durante reset/boot. Nao conecte sinais de 5 V aos GPIOs.

### Componentes e mitigacao de ruido

- ESP32 com display existente e uma segunda placa ESP32 classica.
- Tres sondas NTC e dois medidores de pulso ja usados pelo sistema.
- Divisores com resistores de 10 kOhm e 47 kOhm, conforme a sonda e os valores
  configurados; confirmar tolerancia, potencia e circuito real antes de uso.
- Roteador Wi-Fi 2,4 GHz comum as duas placas; nenhum transceptor adicional.
- Recomendacao de bancada, a validar: desacoplamento local de 100 nF entre
  3V3/GND da unidade de aquisicao e capacitor de 10-100 nF do no ADC ao GND
  local. Isso nao esta presumido como instalado nem substitui aterramento,
  roteamento adequado ou validacao do tempo de resposta.
- Mantenha os fios analogicos curtos, separados dos cabos de rede/contator e
  motores. Evite compartilhar o retorno analogico com correntes de carga.
  Se usar cabo blindado, conecte a blindagem a terra em um unico ponto
  apropriado; nao use a blindagem como retorno do sensor.
- Wi-Fi nao garante imunidade a interferencia. Se o ensaio local mostrar
  perdas, a alternativa recomendada e RS-485 com transceptores apropriados e
  par trancado, nao UART TTL ou I2C direto por 10 m.

## Funcionalidades mantidas

A unidade com display preserva a tela, painel web/AP, botoes, meta, modos
AUTO/MANUAL, agenda diaria, NTP UTC-3, configuracao Wi-Fi, reconhecimento de
falha, controle do rele e persistencia existente. Os graficos ao vivo continuam
temporarios na pagina, e volume de entrada/saida continua persistido.
Os totais sao gravados em intervalos de cinco minutos; uma falta de energia
pode perder pulsos acumulados desde a ultima gravacao, como na V1.

As temperaturas e pulsos agora chegam da placa de aquisicao; nao existe leitura
local dos sensores no display. A perda de telemetria e visivel na tela/painel.
O estado do rele continua sendo apenas o comando enviado.

## Filtro e conversao

A aquisicao coleta a cada 100 ms. Para cada leitura, usa a mediana de cinco
conversoes ADC, converte a resistencia pela equacao Beta do NTC, calcula media
movel de ate tres temperaturas e suaviza com EMA (`alpha = 0.4`). A latencia
adicional esperada e inferior a aproximadamente meio segundo. Leituras ADC
fora da faixa, resistencia/temperatura invalida ou temperatura fora de 0-110 C
sao sinalizadas como invalidas; nao sao substituidas por valores plausiveis.

A equacao do divisor assume resistor ligado a 3V3, NTC ao GND:

```text
Rntc = Rserie * Vadc / (Vcc - Vadc)
1/T = 1/T0 + ln(Rntc/R0)/Beta
```

`analogReadMilliVolts()` e usado para conversao ADC calibrada pelo framework,
mas `SUPPLY_MV` continua configurado em 3300 mV. A referencia/alimentacao do
divisor deve ser medida e a curva do NTC confirmada. Nao use offsets para
compensar erro de curva sem calibracao com termometro de referencia.

## Compilar, gravar e configurar

Requer PlatformIO e dependencias definidas em `platformio.ini`.

```powershell
pio run -e v2-display
pio run -e v2-acquisition
pio run -e v2-display -t upload
pio run -e v2-acquisition -t upload
```

Selecione a porta serial correta para cada placa ao gravar. A unidade de
aquisicao sem credenciais inicia `Boiler-Aquisicao`; conecte o celular/computador
ao AP, abra `http://192.168.4.1`, informe a rede 2,4 GHz e senha. Configure a
placa com display pela interface existente `Boiler-Config` em
`http://192.168.4.1` com a mesma rede. O roteador deve colocar as placas na
mesma LAN; verifique os IPs no serial/painel.

Na V1, a versao estavel pode ser recuperada sem usar os arquivos V2:

```powershell
git switch --detach v1-stable
pio run -e esp32dev
```

Retorne ao trabalho V2 com `git switch v2-development`.

## Roteiro de validacao

1. Compilar os dois ambientes e gravar sem conectar a carga de aquecimento.
2. Confirmar no datasheet/placa os GPIOs e mapear a continuidade dos divisores,
   sensores, medidores e rele com alimentacao desligada.
3. Comparar cada NTC com termometro de referencia em pelo menos dois pontos da
   faixa; ajustar parametros/offsets apenas com medidas documentadas.
4. Conferir mV, resistencia e temperatura no painel; desconectar cada NTC e
   confirmar indicacao de falha.
5. Confirmar pulsos/volumes com fluxo conhecido, incluindo reinicio de cada
   placa e persistencia dos totais.
6. Observar pacotes UDP no mesmo roteador; rejeitar pacote com CRC alterado e
   repeticoes; confirmar atualizacao de sequencia e temperaturas.
7. Interromper Wi-Fi e desligar/reiniciar a aquisicao: verificar rele OFF em
   ate 2,5 s, falha travada, indicacao na tela e impossibilidade de rearmar
   ate cinco amostras validas e reconhecimento manual.
8. Repetir teste a 10 m com contator/cargas operando. Se houver perda de
   pacotes ou leituras instaveis, nao liberar a instalacao por Wi-Fi.
9. Com sensor, rele e protecoes validados, testar agenda, modos, interface,
   persistencia e limite termico sem carga; so depois avaliar operacao
   supervisionada.

## Diferencas entre V1 e V2

| Funcao | V1 estavel (`v1-stable`) | V2 (`v2-development`) |
|---|---|---|
| Placas | ESP32 com display le sensores e controla | ESP32 de aquisicao le sensores/pulsos; ESP32 com display controla |
| Sinais analogicos | Cabos ate a placa com display | Divisores junto da placa perto dos sensores |
| Comunicacao entre unidades | N/A | Wi-Fi local, UDP broadcast com CRC/timeout |
| Filtragem das temperaturas | Media de 16 conversoes por leitura | Mediana de 5, media movel de 3 e EMA |
| Falha de comunicacao | N/A | Desliga e trava o rele |
| Parametros NTC | 10 kOhm/B3950 e 47 kOhm/B3950 provisoria | Mesmos parametros e pinos configurados; ainda requer verificacao/calibracao |
