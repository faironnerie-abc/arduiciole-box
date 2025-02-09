#include "arduiciole_xbee.h"

#include <XBee.h>

typedef struct {
  uint8_t cmd;
} spread_my_sl_t;

uint8_t *payload;

XBee xbee = XBee();

XBeeAddress64 zb_broadcast_addr64 = XBeeAddress64(ZB_BROADCAST_ADDRESS);
spread_my_sl_t spread_payload = {CMD_SWARM};
ZBTxRequest tx_broadcast;

ZBTxRequest tx;
ZBRxResponse rx = ZBRxResponse();
ZBTxStatusResponse tx_status = ZBTxStatusResponse();

XBeeAddress64 zb_swarm[LUCIOLE_VIEW];
uint8_t zb_swarm_offset = 0;
uint8_t zb_swarm_size = 0;

uint8_t assocCmd[] = {'S','L'};
AtCommandRequest atRequest = AtCommandRequest(assocCmd);
AtCommandResponse atResponse = AtCommandResponse();

uint16_t tx_status_expected = 0;

unsigned long last_spread = 0;

void xbee_spread() {
  if (last_spread && millis() < last_spread + XBEE_SPREAD_DELAY) {
    return;
  }

  DEBUG_PRINT_VAL("[XBE] SPREAD @ ", millis())

  xbee.send(tx_broadcast);
  tx_status_expected++;

  last_spread = millis();
}

int xbee_is_in_swarm(XBeeAddress64& addr) {
  for (uint8_t i = 0; i < zb_swarm_size; i++) {
    if (zb_swarm[i].getMsb() == addr.getMsb() && zb_swarm[i].getLsb() == addr.getLsb())
      return 1;
  }

  return 0;
}

void xbee_add_to_swarm(XBeeAddress64& addr) {
  zb_swarm[zb_swarm_offset].setMsb(addr.getMsb());
  zb_swarm[zb_swarm_offset].setLsb(addr.getLsb());
  zb_swarm_offset = (zb_swarm_offset + 1) % LUCIOLE_VIEW;
  zb_swarm_size = min(LUCIOLE_VIEW, zb_swarm_size + 1);

  DEBUG_PRINT_VAL_HEX("[SWA] Nouveau voisin : ", addr.getLsb())
}

/**
 * Configuration du module XBee.
 *
 * La LED intégrée s'éteint jusqu'à ce que la configuration réussisse. Dans le
 * cas contraire, la luciole rentre en mode d'erreur.
 */
void xbee_init(cmd_t *cmd) {
  for (uint8_t i = 0; i < LUCIOLE_VIEW; i++) {
    zb_swarm[i] = XBeeAddress64();
  }

  tx_broadcast = ZBTxRequest(zb_broadcast_addr64, (uint8_t*) &spread_payload, sizeof(spread_my_sl_t));
  tx_broadcast.setOption(ZB_TX_BROADCAST | DISABLE_ACK_OPTION);

  payload = (uint8_t*) cmd;
  tx = ZBTxRequest(zb_swarm[0], payload, sizeof(cmd_t));
  tx.setOption(DISABLE_ACK_OPTION);

  Serial.begin(9600);
  xbee.setSerial(Serial);

  digitalWrite(CLED, LOW);

  xbee.send(atRequest);

  HELPER_BLINK(CLED, 150, 3);

  if (xbee.readPacket(5000)) {
    if (xbee.getResponse().getApiId() == AT_COMMAND_RESPONSE) {
      xbee.getResponse().getAtCommandResponse(atResponse);

      if (atResponse.isOk()) {
        digitalWrite(CLED, HIGH);
        DEBUG_PRINT_VAL_HEX(">>> START ON XBEE ", ((uint32_t*)atResponse.getValue())[0]);
      }
      else {
        error_mode(ERROR_BAD_XBEE_RESPONSE);
      }
    }
    else {
      error_mode(ERROR_BAD_XBEE_RESPONSE);
    }
  }
  else {
    error_mode(ERROR_NO_XBEE_RESPONSE);
  }
}

/*
 * @see arduiciole_xbee.h#xbee_transmit()
 */
void xbee_transmit() {
  DEBUG_DIGITAL_WRITE(11, HIGH)

  for (uint8_t i = 0; i < zb_swarm_size; i++) {
    tx.setAddress64(zb_swarm[i]);
    xbee.send(tx);
    tx_status_expected++;
    DEBUG_PRINT_VAL_HEX("[>TX] TO ", tx.getAddress64().getLsb())
  }

  DEBUG_DIGITAL_WRITE(11, LOW)
}

/*
 * @see arduiciole_xbee.h#xbee_receive()
 */
cmd_t xbee_receive(unsigned long to, uint8_t **data) {
  xbee.readPacket(to);

  if (xbee.getResponse().isAvailable()) {

    DEBUG_DIGITAL_WRITE(6, HIGH)

    if (xbee.getResponse().getApiId() == ZB_RX_RESPONSE) {
      cmd_t cmd = (cmd_t) rx.getData()[0];
      *data = rx.getData();

      DEBUG_DIGITAL_WRITE(7, HIGH)

      xbee.getResponse().getZBRxResponse(rx);

      DEBUG_PRINT_VAL_HEX("[<RX] FROM ", rx.getRemoteAddress64().getLsb())
      DEBUG_DELAY(50)
      DEBUG_DIGITAL_WRITE(6, LOW);
      DEBUG_DIGITAL_WRITE(7, LOW);

      switch(cmd) {
      /*
       * Paquet d'essaimage.
       *
       * Pas besoin de répercuter celui-ci plus haut, mais on le prend en compte
       * pour mettre à jour le voisinage.
       */
      case CMD_SWARM:
        DEBUG_PRINT_VAL("[CMD] SWARM @ ", millis())

        if (!xbee_is_in_swarm(rx.getRemoteAddress64())) {
          if (zb_swarm_size < LUCIOLE_VIEW) {
            xbee_add_to_swarm(rx.getRemoteAddress64());
          }
          else if (zb_swarm_size < LUCIOLE_VIEW || random(1000) < LUCIOLE_ADD_IN_SWARM_PROB * 1000) {
            xbee_add_to_swarm(rx.getRemoteAddress64());
          }
        }

        break;
      /*
       * Remise à zéro du voisinage dans le cas d'une CMD_RESET.
       */
      case CMD_RESET:
        zb_swarm_size = 0;
        zb_swarm_offset = 0;
      default:
        return cmd;
      }
    }
    /*
     * Cas où l'on recevrait un confirmation d'envoi.
     */
    else if (xbee.getResponse().getApiId() == ZB_TX_STATUS_RESPONSE) {
      tx_status_expected--;
#ifdef DEBUG
      altSoftSerial.print("[XBE] TX_STATUS ");

      xbee.getResponse().getZBTxStatusResponse(tx_status);

      if (tx_status.isSuccess()) {
        altSoftSerial.print("[SUCCESS] ");
      }
      else {
        altSoftSerial.print("[FAILED] ");
      }

      altSoftSerial.print(tx_status_expected);
      altSoftSerial.println(" restant(s)");

      return CMD_TX_STATUS;
#endif
    }
#ifdef DEBUG
    else {
      altSoftSerial.print("[XBE] API ID inconnu : ");
      altSoftSerial.println(xbee.getResponse().getApiId(), HEX);
    }

    delay(50);
    digitalWrite(6, LOW);
#endif
  }

  return CMD_NONE;
}
