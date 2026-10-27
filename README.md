# Controlador de boiler — ESP32 T-Display

Firmware inicial para T-Display classica ESP32 com ST7789 135 x 240.
A TENSTAR ainda precisa ser conferida fisicamente: nao foi possivel acessar o anuncio.
Nao foi validado em hardware. Nao grave/conecte a carga antes de confirmar a pinagem e a polaridade do modulo de rele.

## Conexoes de baixa tensao

- GPIO27: sinal para a interface de rele existente. Padrao ativo em LOW.
- GPIO32: leitura analogica ADC1 do NTC.
- GPIO25: entrada dos pulsos de fluxo da entrada das placas; GPIO26: entrada dos pulsos da saida. Ambos os pinos sao entradas digitais com interrupcao.
- Temperatura por NTC nas placas: GPIO33 (entrada) e GPIO36 (saida, somente entrada). Cada sensor usa divisor independente: 3V3 — resistor 47 kohms — pino ADC — NTC — GND. A sonda medida em cerca de 48 kohms a temperatura ambiente aparenta ser NTC; o firmware usa provisoriamente NTC nominal de 47 kohms e beta 3950, nao confirmados. Os graus sao apenas estimativas ate calibracao e nao entram no controle. O painel web mostra tambem milivolts e resistencia calculada para diagnostico.
- Para o primeiro teste, alimente cada YF-B6 com 5 V regulados e GND comum ao ESP32, somente depois de confirmar os fios no datasheet. A especificacao informa nivel alto de pulso acima de 4,7 V: nao ligue a saida diretamente ao GPIO. Para saida push-pull de 5 V, use um divisor por sensor (12 kohms entre saida e GPIO; 22 kohms entre GPIO e GND). Nao alimente o sensor acima de 5 V com esse divisor.
- Se a saida nao for push-pull de 5 V, ou a identificacao dos fios nao estiver clara, pare e confirme o tipo antes de ligar; a interface pode precisar ser diferente. GPIO25/26 sao apenas sinais digitais de fluxo, nao entradas para temperatura.
- A vazao usa 396 pulsos/L como valor nominal do anuncio, atualizada a cada 2 segundos. Volume total e vazao sao exibidos no painel web; os totais de pulsos ficam salvos na memoria do ESP32 e continuam apos reiniciar. O firmware grava os totais a cada 5 minutos para limitar escritas; um corte de energia pode perder pulsos acumulados desde a ultima gravacao. A leitura 0.0 significa que nao foram contados pulsos e nao distingue fluxo parado de falha de ligacao. A temperatura interna existente no boiler segue sendo a referencia do controle.
- Divisor: **3V3 — resistor 10 kohms 1% — GPIO32 — NTC 10 kohms B3950 — GND**.
- Nunca aplicar 5 V ao ADC. Nao compartilhar o sensor do termostato independente.
- Usar interface compativel com logica de 3,3 V; conferir alimentacao e referencia de terra conforme o modulo.
- Para manter o rele desligado durante reset/boot (antes do firmware), a interface precisa de polarizacao externa adequada: pull-up para ativo LOW ou pull-down para ativo HIGH, compativel com 3,3 V.

Pinos, polaridade, valores do NTC e tensao medida de 3V3 ficam em `include/config.h`.
Calibrar a leitura contra um termometro de referencia na faixa de trabalho. Os valores do sensor foram assumidos da descricao, sem datasheet da sonda.
Confirmar limite termico e instalacao permitida da sonda, especialmente em reservatorio pressurizado.
Preservar o termostato independente existente.

## Primeiro acesso

1. Conferir as conexoes sem a carga de aquecimento e compilar/gravar pelo PlatformIO.
2. Conectar o celular a rede **Boiler-Config**, senha **boiler123** (alterar em config.h).
3. Abrir **http://192.168.4.1**. Manter a conexao mesmo que o celular informe ausencia de internet.
4. Cadastrar o Wi-Fi de casa para sincronizacao NTP. Fuso fixo UTC-3.
5. Configurar temperatura (20 a 70 C) e inicio/fim diarios, e habilitar o controle.
6. Verificar leitura, saida e polaridade do rele sem a carga antes de colocar em operacao.

O painel usa a senha da rede como controle de acesso; nao possui login separado, HTTPS ou acesso remoto. Nao expor a porta na internet. Qualquer dispositivo na rede local pode acessar o painel.

## Comportamento

- A tela principal preserva a leitura grande do sensor que fica dentro do boiler e a meta; abaixo, reserva duas colunas para temperatura e fluxo na entrada e na saida das placas. As temperaturas dos NTCs das placas sao leituras aproximadas usando parametros provisórios; vazoes sao calculadas pelos pulsos nos GPIO25/26. Nenhuma leitura dos medidores participa do controle.
- O painel web atualiza estado e leituras a cada 2 segundos, mostra os volumes acumulados persistentes de entrada e saida e desenha graficos ao vivo da temperatura do boiler/meta e das vazoes, com ate 4 minutos de historico enquanto a pagina fica aberta. O historico dos graficos e temporario e nao e gravado.
- A linha inferior alterna a cada 5 segundos entre o estado do controle, a agenda ou modo manual e a conexao Wi-Fi/endereco IP. Durante o aquecimento, ou em falha, a mensagem correspondente fica fixa.
- Botoes integrados: GPIO35 aumenta a meta em 0,5 C; GPIO0 diminui, dentro de 20 a 70 C. Nao habilitam o aquecimento.
- A tela reduz o brilho apos 20 segundos sem tocar nos botoes, conforme `DISPLAY_IDLE_BRIGHTNESS`. O primeiro toque curto apenas acende; os seguintes ajustam ao soltar. Cada toque curto gera um passo, sem repeticao ao segurar. Dois botoes juntos nao ajustam.
- Segurar GPIO35 (+) por 2 segundos alterna MANUAL/AUTO. Ao entrar no MANUAL, guarda a habilitacao anterior do AUTO e habilita o controle; ao voltar ao AUTO, restaura essa habilitacao e segue a agenda. A troca desliga a saida antes de reavaliar e respeita a pausa para religar. Um toque longo nao altera a meta e funciona mesmo com a tela em repouso; e necessario soltar antes de alternar novamente. No MANUAL, mantem a temperatura sem depender da agenda ou NTP. Falha do sensor e limite termico continuam valendo nos dois modos. GPIO0 longo nao tem acao.
- A tela e a pagina web mostram o modo atual. Ao reiniciar volta para AUTO desabilitado. O botao web Desligar controle desabilita o aquecimento em qualquer modo.
- Os ajustes pelos botoes sao salvos apos 1,5 segundo sem novas alteracoes. Uma queda de energia antes disso pode perder o ultimo ajuste.
- Em `include/config.h`, `DISPLAY_IDLE_BRIGHTNESS = 0` apaga a luz; usar por exemplo `8` para brilho fraco. `DISPLAY_BRIGHTNESS` regula o brilho ativo (0..255) e `DISPLAY_TIMEOUT_MS` o tempo. Apenas a iluminacao apaga: controle e Wi-Fi continuam funcionando.
- Pinagem dos botoes baseada na [T-Display classica oficial](https://github.com/Xinyuan-LilyGO/TTGO-T-Display). GPIO35 depende do pull-up externo da placa; GPIO0 tambem participa do boot, portanto solte os botoes ao ligar/reiniciar.

- Uma janela diaria, inclusive atravessando a meia-noite. Inicio igual ao fim significa nenhuma janela.
- Liga em temperatura desejada menos 2 C; desliga ao atingir a temperatura desejada.
- Aguarda pelo menos 10 segundos antes de religar; desligamentos nao aguardam.
- Sensor invalido ou leitura de 75 C ou mais retém falha e desliga a saida. Reconhecimento manual exige cinco leituras validas e temperatura abaixo de 73 C.
- No AUTO, sem horario sincronizado desde o boot, nao aquece. Apos sincronizacao o relogio continua localmente se perder Wi-Fi, sujeito a deriva.
- Reinicio deixa o aquecimento desabilitado; temperatura, janela e credenciais Wi-Fi permanecem salvas. Habilitar novamente pelo painel.
- Nao existe modo de acionamento forcado que ignore temperatura/horario.
- O estado exibido e o comando enviado ao rele, sem confirmacao fisica do contator.

## Compilacao

`pio run` — compilar. `pio run -t upload` — gravar. `pio device monitor` — serial 115200.

Configuracao da tela baseada no [exemplo oficial LILYGO](https://github.com/Xinyuan-LilyGO/TTGO-T-Display/blob/master/TFT_eSPI/User_Setups/Setup25_TTGO_T_Display.h).
Leitura calibrada em milivolts com [API ADC Espressif](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/adc.html).
