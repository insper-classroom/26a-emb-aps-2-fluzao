# Controle Customizado — Traffic Rider

Projeto integrador da disciplina de Sistemas Embarcados (Insper). Desenvolvimento de um controle físico dedicado ao jogo **Traffic Rider** combinando os laboratórios *expert* de **IA (Edge Impulse)** e **RTOS (FreeRTOS SMP)** em uma única plataforma — Raspberry Pi Pico 2 (RP2350).

> **Status:** entrega prévia — design e arquitetura. Implementação em andamento.

---

## 1. Jogo

[**Traffic Rider**](https://www.crazygames.com/game/traffic-rider-vvq) é um jogo de corrida de moto em primeira pessoa, jogável em navegador no PC. O jogador pilota uma moto em alta velocidade desviando do trânsito, com mecânicas de aceleração, frenagem, esterçamento e wheelie.

**Mapeamento de teclas do jogo utilizado neste controle:**

| Tecla | Função |
|---|---|
| `↑` | Acelerar |
| `↓` | Frear |
| `← / →` | Esterçar (substituído pelo IMU) |
| `H` | Buzina |
| `Y` | Wheelie (acionado por gesto via IA) |
| `P` | Pausar |

---

## 2. Ideia do controle

A proposta é um controle no formato de **guidão de moto reduzido**, segurado com as duas mãos, onde a inclinação lateral do corpo do controle (eixo de roll do IMU) controla diretamente o esterçamento da moto no jogo. Botões físicos posicionados nas extremidades atendem aos comandos discretos (acelerar, frear, buzina e pause).

O diferencial do controle é o **wheelie por gesto**: ao invés de um botão dedicado, o jogador realiza o movimento físico de puxar o guidão pra trás, e um modelo de IA embarcado classifica o gesto e dispara a tecla `Y` no jogo. Isso aproxima a interação da pilotagem real e dá uso justificável ao lab de IA.

### Esboço mecânico

> Esboço/scratch a ser anexado nos próximos commits.

Direcionamento: formato de tubo/barra curto com pegadas nas extremidades, simulando a posição de pilotagem. Eletrônica (Pico 2, MPU6050, bateria) embutidas no corpo central. Botões posicionados próximos ao polegar de cada mão.

---

## 3. Inputs e Outputs

### Inputs

| Componente | Interface | Função | Tratamento |
|---|---|---|---|
| MPU6050 (IMU) | I²C | Aceleração + giroscópio para steering contínuo e detecção de gestos | Polling em task @ 100 Hz |
| Botão Acelerar | GPIO digital | Tecla `↑` | ISR + semáforo + debounce |
| Botão Frear | GPIO digital | Tecla `↓` | ISR + semáforo + debounce |
| Botão Buzina | GPIO digital | Tecla `H` | ISR + semáforo + debounce |
| Botão Pause | GPIO digital | Tecla `P` | ISR + semáforo + debounce |

### Outputs

| Componente | Interface | Função |
|---|---|---|
| LED RGB | GPIO/PWM | Indicador de status: conectado, desconectado, calibrando, ativo |
| USB | Serial UART (USB-CDC) | Comunicação com o PC via datagrama |

### Decisões pendentes (a confirmar até a entrega final)

- **Bateria** vs alimentação USB direta — definir se vai integrar gerenciamento de bateria (acumulativo +0.5)
- **Throttle analógico** — substituir o botão de acelerar por potenciômetro/gatilho ADC para acelerador progressivo (acumulativo ADC + IMU +0.5)
- **Motor de vibração** — adicionar feedback háptico para eventos locais (acumulativo +0.5)

---

## 4. Protocolo

**Comunicação serial via datagrama de 4 bytes (UART sobre USB-CDC).**

O controle se comunica com o PC por uma porta serial. Um programa Python rodando no PC lê a serial, interpreta os datagramas recebidos e gera os eventos de teclado para o jogo. A abordagem reaproveita a estrutura do laboratório [`python-mouse`](https://github.com/insper-embarcados/python-mouse) da disciplina, com o script Python adaptado para emitir teclas em vez de mover o mouse.

### Formato do datagrama

Cada pacote é composto por **4 bytes** (8 bits cada), na ordem:

```
AXIS   VAL_1   VAL_0   EOP
```

| Campo | Descrição |
|---|---|
| `AXIS` | Canal do dado — `0` ou `1` |
| `VAL_1` | Byte mais significativo (MSB) do valor |
| `VAL_0` | Byte menos significativo (LSB) do valor |
| `EOP` | `0xFF` (`-1`) — marca o fim do pacote |

O valor transmitido (`VAL_1`:`VAL_0`) é um inteiro de **16 bits com sinal** (complemento de dois), permitindo valores positivos e negativos.

**Exemplo — valor 845 no canal 0** (845 = `00000011 01001101`):

```
00000000   01001101   00000011   11111111
  AXIS                            EOP
```

**Exemplo — valor -55 no canal 1** (-55 = `11111111 11001001`):

```
00000001   11001001   11111111   11111111
  AXIS                            EOP
```

> **Verificar a ordem dos bytes:** o rótulo do formato lista `VAL_1` (MSB) antes de `VAL_0` (LSB), mas nos exemplos do lab o byte imediatamente após `AXIS` é o **menos significativo (LSB)**. Antes de fixar a implementação do firmware, confirmar a ordem exata esperada pelo arquivo `python/main.py` do repositório `python-mouse`.

### Mapeamento dos canais

O controle utiliza os dois canais do datagrama da seguinte forma:

| `AXIS` | Significado | Faixa do valor |
|---|---|---|
| `0` | Esterçamento (roll do IMU) | `-255` a `+255`, com zona morta no centro |
| `1` | Estado de botões e gestos (bitmask) | bits individuais — ver abaixo |

**Canal 0 — esterçamento:** o ângulo de roll calculado pela `fusion_task` é reescalado para a faixa `-255 ... +255`. Uma **zona morta** em torno do zero evita o envio de comando por ruído ou tremor quando o controle está nivelado. O script Python converte esse valor em pulsos modulados das setas `←` / `→`.

**Canal 1 — bitmask de eventos:** cada bit do valor de 16 bits representa o estado de um botão ou gesto:

| Bit | Evento | Tecla no jogo |
|---|---|---|
| 0 | Acelerar | `↑` |
| 1 | Frear | `↓` |
| 2 | Buzina | `H` |
| 3 | Pause | `P` |
| 4 | Wheelie (gesto IA) | `Y` |

Bit em `1` = pressionado/ativo, bit em `0` = solto. O script Python decodifica o bitmask e mantém as teclas correspondentes pressionadas.

> Essa organização mantém o datagrama **exatamente no formato do lab** (`AXIS` ∈ {`0`, `1`}): o canal "eixo Y" é reaproveitado como canal de botões/gestos em vez de um segundo eixo analógico.

---

## 5. Experts aplicados

### 5.1 IA — Edge Impulse

Um modelo de classificação de gestos roda localmente na Pico 2 utilizando dados do MPU6050. O modelo é treinado na plataforma Edge Impulse e exportado como biblioteca C++ embarcada no firmware.

**Classes do modelo:**

| Classe | Significado | Ação |
|---|---|---|
| `idle` | Controle parado, posição neutra | Nenhuma |
| `steering` | Inclinação lateral sustentada (curva normal) | Nenhuma (tratado pela malha contínua de fusion) |
| `wheelie` | Movimento brusco de pull-back do controle | Ativa o bit de wheelie no datagrama (tecla `Y`) |

**Pipeline:** janela móvel de samples do MPU6050 alimenta inferência periódica (~5 Hz). Threshold de confiança configurável para evitar falsos positivos durante curvas agressivas.

Base de código: [edgeimpulse-runner para Pico 2](https://github.com/insper-embarcados/edgeimpulse-runner) com substituição dos artefatos `tflite-model`, `model-parameters` e `edge-impulse-sdk` pelo modelo treinado.

### 5.2 RTOS — FreeRTOS SMP

Firmware estruturado em tasks com filas e semáforos (sem variáveis globais). O modo **SMP** é ativado para distribuir as tasks entre os dois cores do RP2350, isolando a inferência de IA (carga maior) do pipeline crítico de input e transmissão serial (baixa latência).

**Métricas a reportar (entregáveis do lab):**

| Métrica | imu_task | fusion_task | ai_task | button_task | uart_task |
|---|---|---|---|---|---|
| WCET | TBD | TBD | TBD | TBD | TBD |
| Jitter | TBD | TBD | TBD | TBD | TBD |
| Deadline Miss Rate | TBD | TBD | TBD | TBD | TBD |
| Stack Usage | TBD | TBD | TBD | TBD | TBD |

Medições serão realizadas duas vezes: **Single Core** e **SMP (2 cores)**, com tabela comparativa no relatório final.

---

## 6. Diagrama de blocos do firmware
<img width="1600" height="872" alt="image" src="https://github.com/user-attachments/assets/1fb867bc-fc3a-449d-8c85-8c6b38a7b7b4" />
---

## 7. Cronograma de entregas

- [x] **Entrega prévia (15/05):** README com design e arquitetura
- [ ] **Implementação:** firmware em FreeRTOS, transmissão serial via datagrama
- [ ] **Script Python:** adaptação do `python-mouse` para emitir teclas do jogo
- [ ] **Coleta de dados e treinamento:** modelo Edge Impulse
- [ ] **Deploy do modelo:** integração na Pico 2 via `edge-impulse-runner`
- [ ] **Medições de RTOS:** Single Core + SMP, tabelas e gráficos
- [ ] **Hardware:** PCB/protótipo mecânico e bateria
- [ ] **Vídeo e entrega final**

---

## 8. Acumulativos pretendidos

Itens do regulamento da APS que o projeto pretende cumprir (a confirmar durante o desenvolvimento):

- [ ] ADC + IMU em conjunto (substituir botão de acelerar por gatilho analógico)
- [ ] Componente fora da sala de aula (motor de vibração / encoder / etc.)
- [ ] Botão de macro (gravação e reprodução de sequência)
- [ ] Vídeo top/stonks nível Kickstarter
- [ ] Controle recebe info do PC (vibração no crash)
- [ ] Calibração guiada via LED
- [ ] Háptico (motor de vibração)
- [ ] Gerenciamento de bateria

---

## 9. Referências

- [Repositório base — Edge Impulse Runner para Pico 2](https://github.com/insper-embarcados/edgeimpulse-runner)
- [Repositório base — Data Forwarder para Edge Impulse](https://github.com/insper-embarcados/edgeimpulse-dataforwarding)
- [Repositório base — Python Mouse (leitura de UART)](https://github.com/insper-embarcados/python-mouse)
- [Documentação oficial — Continuous Motion Recognition (Edge Impulse)](https://docs.edgeimpulse.com/docs/tutorials/end-to-end-tutorials/continuous-motion-recognition)
- [FreeRTOS SMP — documentação oficial](https://www.freertos.org/symmetric-multiprocessing-introduction.html)
- [FreeRTOS — Queue com struct (guia da disciplina)](https://insper-embarcados.github.io/site/guides/freertos-queue-advanced.html)
- [Traffic Rider — CrazyGames (versão web)](https://www.crazygames.com/game/traffic-rider-vvq)

---

## 10. Equipe

> *Preencher com os membros da equipe.*
