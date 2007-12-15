/**
  * This file contains functions used in USB interface module.
  */
#include <linux/delay.h>
#include <linux/moduleparam.h>
#include <linux/firmware.h>
#include <linux/netdevice.h>
#include <linux/usb.h>

#define DRV_NAME "usb8xxx"

#include "host.h"
#include "decl.h"
#include "defs.h"
#include "dev.h"
#include "cmd.h"
#include "if_usb.h"

#define MESSAGE_HEADER_LEN	4

static char *lbs_fw_name = "usb8388.bin";
module_param_named(fw_name, lbs_fw_name, charp, 0644);

static struct usb_device_id if_usb_table[] = {
	/* Enter the device signature inside */
	{ USB_DEVICE(0x1286, 0x2001) },
	{ USB_DEVICE(0x05a3, 0x8388) },
	{}	/* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, if_usb_table);

static void if_usb_receive(struct urb *urb);
static void if_usb_receive_fwload(struct urb *urb);
static int if_usb_prog_firmware(struct usb_card_rec *cardp);
static int if_usb_host_to_card(struct lbs_private *priv,
	u8 type,
	u8 *payload,
	u16 nb);
static int if_usb_get_int_status(struct lbs_private *priv, u8 *);
static int if_usb_read_event_cause(struct lbs_private *);
static int usb_tx_block(struct usb_card_rec *cardp, u8 *payload, u16 nb);
static void if_usb_free(struct usb_card_rec *cardp);
static int if_usb_submit_rx_urb(struct usb_card_rec *cardp);
static int if_usb_reset_device(struct usb_card_rec *cardp);

/**
 *  @brief  call back function to handle the status of the URB
 *  @param urb 		pointer to urb structure
 *  @return 	   	N/A
 */
static void if_usb_write_bulk_callback(struct urb *urb)
{
	struct usb_card_rec *cardp = (struct usb_card_rec *) urb->context;

	/* handle the transmission complete validations */

	if (urb->status == 0) {
		struct lbs_private *priv = cardp->priv;

		/*
		lbs_deb_usbd(&urb->dev->dev, "URB status is successfull\n");
		lbs_deb_usbd(&urb->dev->dev, "Actual length transmitted %d\n",
		       urb->actual_length);
		*/

		/* Used for both firmware TX and regular TX.  priv isn't
		 * valid at firmware load time.
		 */
		if (priv)
			lbs_host_to_card_done(priv);
	} else {
		/* print the failure status number for debug */
		lbs_pr_info("URB in failure status: %d\n", urb->status);
	}

	return;
}

/**
 *  @brief  free tx/rx urb, skb and rx buffer
 *  @param cardp	pointer usb_card_rec
 *  @return 	   	N/A
 */
static void if_usb_free(struct usb_card_rec *cardp)
{
	lbs_deb_enter(LBS_DEB_USB);

	/* Unlink tx & rx urb */
	usb_kill_urb(cardp->tx_urb);
	usb_kill_urb(cardp->rx_urb);

	usb_free_urb(cardp->tx_urb);
	cardp->tx_urb = NULL;

	usb_free_urb(cardp->rx_urb);
	cardp->rx_urb = NULL;

	kfree(cardp->bulk_out_buffer);
	cardp->bulk_out_buffer = NULL;

	lbs_deb_leave(LBS_DEB_USB);
}

static void if_usb_set_boot2_ver(struct lbs_private *priv)
{
	struct cmd_ds_set_boot2_ver b2_cmd;

	b2_cmd.action = 0;
	b2_cmd.version = priv->boot2_version;

	if (lbs_cmd(priv, CMD_SET_BOOT2_VER, b2_cmd, NULL, 0))
		lbs_deb_usb("Setting boot2 version failed\n");
}

static void if_usb_fw_timeo(unsigned long priv)
{
	struct usb_card_rec *cardp = (void *)priv;

	if (cardp->fwdnldover) {
		lbs_deb_usb("Download complete, no event. Assuming success\n");
	} else {
		lbs_pr_err("Download timed out\n");
		cardp->surprise_removed = 1;
	}
	wake_up(&cardp->fw_wq);
}
/**
 *  @brief sets the configuration values
 *  @param ifnum	interface number
 *  @param id		pointer to usb_device_id
 *  @return 	   	0 on success, error code on failure
 */
static int if_usb_probe(struct usb_interface *intf,
			const struct usb_device_id *id)
{
	struct usb_device *udev;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	struct lbs_private *priv;
	struct usb_card_rec *cardp;
	int i;

	udev = interface_to_usbdev(intf);

	cardp = kzalloc(sizeof(struct usb_card_rec), GFP_KERNEL);
	if (!cardp) {
		lbs_pr_err("Out of memory allocating private data.\n");
		goto error;
	}

	setup_timer(&cardp->fw_timeout, if_usb_fw_timeo, (unsigned long)cardp);
	init_waitqueue_head(&cardp->fw_wq);
								     
	cardp->udev = udev;
	iface_desc = intf->cur_altsetting;

	lbs_deb_usbd(&udev->dev, "bcdUSB = 0x%X bDeviceClass = 0x%X"
	       " bDeviceSubClass = 0x%X, bDeviceProtocol = 0x%X\n",
		     le16_to_cpu(udev->descriptor.bcdUSB),
		     udev->descriptor.bDeviceClass,
		     udev->descriptor.bDeviceSubClass,
		     udev->descriptor.bDeviceProtocol);

	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i].desc;
		if ((endpoint->bEndpointAddress & USB_ENDPOINT_DIR_MASK)
		    && ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) ==
			USB_ENDPOINT_XFER_BULK)) {
			/* we found a bulk in endpoint */
			lbs_deb_usbd(&udev->dev, "Bulk in size is %d\n",
				     le16_to_cpu(endpoint->wMaxPacketSize));
			if (!(cardp->rx_urb = usb_alloc_urb(0, GFP_KERNEL))) {
				lbs_deb_usbd(&udev->dev,
				       "Rx URB allocation failed\n");
				goto dealloc;
			}
			cardp->bulk_in_size =
				le16_to_cpu(endpoint->wMaxPacketSize);
			cardp->bulk_in_endpointAddr =
			    (endpoint->
			     bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);
			lbs_deb_usbd(&udev->dev, "in_endpoint = %d\n",
			       endpoint->bEndpointAddress);
		}

		if (((endpoint->
		      bEndpointAddress & USB_ENDPOINT_DIR_MASK) ==
		     USB_DIR_OUT)
		    && ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) ==
			USB_ENDPOINT_XFER_BULK)) {
			/* We found bulk out endpoint */
			if (!(cardp->tx_urb = usb_alloc_urb(0, GFP_KERNEL))) {
				lbs_deb_usbd(&udev->dev,
				       "Tx URB allocation failed\n");
				goto dealloc;
			}

			cardp->bulk_out_size =
				le16_to_cpu(endpoint->wMaxPacketSize);
			lbs_deb_usbd(&udev->dev,
				     "Bulk out size is %d\n",
				     le16_to_cpu(endpoint->wMaxPacketSize));
			cardp->bulk_out_endpointAddr =
			    endpoint->bEndpointAddress;
			lbs_deb_usbd(&udev->dev, "out_endpoint = %d\n",
				    endpoint->bEndpointAddress);
			cardp->bulk_out_buffer =
			    kmalloc(MRVDRV_ETH_TX_PACKET_BUFFER_SIZE,
				    GFP_KERNEL);

			if (!cardp->bulk_out_buffer) {
				lbs_deb_usbd(&udev->dev,
				       "Could not allocate buffer\n");
				goto dealloc;
			}
		}
	}

	/* Upload firmware */
	cardp->rinfo.cardp = cardp;
	if (if_usb_prog_firmware(cardp)) {
		lbs_deb_usbd(&udev->dev, "FW upload failed");
		goto err_prog_firmware;
	}

	if (!(priv = lbs_add_card(cardp, &udev->dev)))
		goto err_prog_firmware;

	cardp->priv = priv;
	cardp->priv->fw_ready = 1;

	priv->hw_host_to_card = if_usb_host_to_card;
	priv->hw_get_int_status = if_usb_get_int_status;
	priv->hw_read_event_cause = if_usb_read_event_cause;
	priv->boot2_version = udev->descriptor.bcdDevice;

	if_usb_submit_rx_urb(cardp);

	if (lbs_start_card(priv))
		goto err_start_card;

	if_usb_set_boot2_ver(priv);

	usb_get_dev(udev);
	usb_set_intfdata(intf, cardp);

	return 0;

err_start_card:
	lbs_remove_card(priv);
err_prog_firmware:
	if_usb_reset_device(cardp);
dealloc:
	if_usb_free(cardp);

error:
	return -ENOMEM;
}

/**
 *  @brief free resource and cleanup
 *  @param intf		USB interface structure
 *  @return 	   	N/A
 */
static void if_usb_disconnect(struct usb_interface *intf)
{
	struct usb_card_rec *cardp = usb_get_intfdata(intf);
	struct lbs_private *priv = (struct lbs_private *) cardp->priv;

	lbs_deb_enter(LBS_DEB_MAIN);

	/* Update Surprise removed to TRUE */
	cardp->surprise_removed = 1;

	if (priv) {

		priv->surpriseremoved = 1;
		lbs_stop_card(priv);
		lbs_remove_card(priv);
	}

	/* this is (apparently?) necessary for future usage of the device */
	lbs_prepare_and_send_command(priv, CMD_802_11_RESET, CMD_ACT_HALT,
			0, 0, NULL);

	/* Unlink and free urb */
	if_usb_free(cardp);

	usb_set_intfdata(intf, NULL);
	usb_put_dev(interface_to_usbdev(intf));

	lbs_deb_leave(LBS_DEB_MAIN);
}

/**
 *  @brief  This function download FW
 *  @param priv		pointer to struct lbs_private
 *  @return 	   	0
 */
static int if_usb_send_fw_pkt(struct usb_card_rec *cardp)
{
	struct FWData *fwdata;
	struct fwheader *fwheader;
	u8 *firmware = cardp->fw->data;

	fwdata = kmalloc(sizeof(struct FWData), GFP_ATOMIC);

	if (!fwdata)
		return -1;

	fwheader = &fwdata->fwheader;

	if (!cardp->CRC_OK) {
		cardp->totalbytes = cardp->fwlastblksent;
		cardp->fwseqnum = cardp->lastseqnum - 1;
	}

	/*
	lbs_deb_usbd(&cardp->udev->dev, "totalbytes = %d\n",
		    cardp->totalbytes);
	*/

	memcpy(fwheader, &firmware[cardp->totalbytes],
	       sizeof(struct fwheader));

	cardp->fwlastblksent = cardp->totalbytes;
	cardp->totalbytes += sizeof(struct fwheader);

	/* lbs_deb_usbd(&cardp->udev->dev,"Copy Data\n"); */
	memcpy(fwdata->data, &firmware[cardp->totalbytes],
	       le32_to_cpu(fwdata->fwheader.datalength));

	/*
	lbs_deb_usbd(&cardp->udev->dev,
		    "Data length = %d\n", le32_to_cpu(fwdata->fwheader.datalength));
	*/

	cardp->fwseqnum = cardp->fwseqnum + 1;

	fwdata->seqnum = cpu_to_le32(cardp->fwseqnum);
	cardp->lastseqnum = cardp->fwseqnum;
	cardp->totalbytes += le32_to_cpu(fwdata->fwheader.datalength);

	if (fwheader->dnldcmd == cpu_to_le32(FW_HAS_DATA_TO_RECV)) {
		/*
		lbs_deb_usbd(&cardp->udev->dev, "There are data to follow\n");
		lbs_deb_usbd(&cardp->udev->dev,
			    "seqnum = %d totalbytes = %d\n", cardp->fwseqnum,
			    cardp->totalbytes);
		*/
		memcpy(cardp->bulk_out_buffer, fwheader, FW_DATA_XMIT_SIZE);
		usb_tx_block(cardp, cardp->bulk_out_buffer, FW_DATA_XMIT_SIZE);

	} else if (fwdata->fwheader.dnldcmd == cpu_to_le32(FW_HAS_LAST_BLOCK)) {
		/*
		lbs_deb_usbd(&cardp->udev->dev,
			    "Host has finished FW downloading\n");
		lbs_deb_usbd(&cardp->udev->dev,
			    "Donwloading FW JUMP BLOCK\n");
		*/
		memcpy(cardp->bulk_out_buffer, fwheader, FW_DATA_XMIT_SIZE);
		usb_tx_block(cardp, cardp->bulk_out_buffer, FW_DATA_XMIT_SIZE);
		cardp->fwfinalblk = 1;
	}

	/*
	lbs_deb_usbd(&cardp->udev->dev,
		    "The firmware download is done size is %d\n",
		    cardp->totalbytes);
	*/

	kfree(fwdata);

	return 0;
}

static int if_usb_reset_device(struct usb_card_rec *cardp)
{
	struct cmd_ds_command *cmd = (void *)&cardp->bulk_out_buffer[4];
	int ret;

	lbs_deb_enter(LBS_DEB_USB);

	*(__le32 *)cardp->bulk_out_buffer = cpu_to_le32(CMD_TYPE_REQUEST);

	cmd->command = cpu_to_le16(CMD_802_11_RESET);
	cmd->size = cpu_to_le16(sizeof(struct cmd_ds_802_11_reset) + S_DS_GEN);
	cmd->result = cpu_to_le16(0);
	cmd->seqnum = cpu_to_le16(0x5a5a);
	cmd->params.reset.action = cpu_to_le16(CMD_ACT_HALT);
	usb_tx_block(cardp, cardp->bulk_out_buffer, 4 + S_DS_GEN + sizeof(struct cmd_ds_802_11_reset));

	msleep(100);
	ret = usb_reset_device(cardp->udev);
	msleep(100);

	lbs_deb_leave_args(LBS_DEB_USB, "ret %d", ret);

	return ret;
}

/**
 *  @brief This function transfer the data to the device.
 *  @param priv 	pointer to struct lbs_private
 *  @param payload	pointer to payload data
 *  @param nb		data length
 *  @return 	   	0 or -1
 */
static int usb_tx_block(struct usb_card_rec *cardp, u8 * payload, u16 nb)
{
	int ret = -1;

	/* check if device is removed */
	if (cardp->surprise_removed) {
		lbs_deb_usbd(&cardp->udev->dev, "Device removed\n");
		goto tx_ret;
	}

	usb_fill_bulk_urb(cardp->tx_urb, cardp->udev,
			  usb_sndbulkpipe(cardp->udev,
					  cardp->bulk_out_endpointAddr),
			  payload, nb, if_usb_write_bulk_callback, cardp);

	cardp->tx_urb->transfer_flags |= URB_ZERO_PACKET;

	if ((ret = usb_submit_urb(cardp->tx_urb, GFP_ATOMIC))) {
		/*  transfer failed */
		lbs_deb_usbd(&cardp->udev->dev, "usb_submit_urb failed: %d\n", ret);
		ret = -1;
	} else {
		/* lbs_deb_usbd(&cardp->udev->dev, "usb_submit_urb success\n"); */
		ret = 0;
	}

tx_ret:
	return ret;
}

static int __if_usb_submit_rx_urb(struct usb_card_rec *cardp,
				  void (*callbackfn)(struct urb *urb))
{
	struct sk_buff *skb;
	struct read_cb_info *rinfo = &cardp->rinfo;
	int ret = -1;

	if (!(skb = dev_alloc_skb(MRVDRV_ETH_RX_PACKET_BUFFER_SIZE))) {
		lbs_pr_err("No free skb\n");
		goto rx_ret;
	}

	rinfo->skb = skb;

	/* Fill the receive configuration URB and initialise the Rx call back */
	usb_fill_bulk_urb(cardp->rx_urb, cardp->udev,
			  usb_rcvbulkpipe(cardp->udev,
					  cardp->bulk_in_endpointAddr),
			  (void *) (skb->tail + (size_t) IPFIELD_ALIGN_OFFSET),
			  MRVDRV_ETH_RX_PACKET_BUFFER_SIZE, callbackfn,
			  rinfo);

	cardp->rx_urb->transfer_flags |= URB_ZERO_PACKET;

	/* lbs_deb_usbd(&cardp->udev->dev, "Pointer for rx_urb %p\n", cardp->rx_urb); */
	if ((ret = usb_submit_urb(cardp->rx_urb, GFP_ATOMIC))) {
		/* handle failure conditions */
		lbs_deb_usbd(&cardp->udev->dev, "Submit Rx URB failed: %d\n", ret);
		kfree_skb(skb);
		rinfo->skb = NULL;
		ret = -1;
	} else {
		/* lbs_deb_usbd(&cardp->udev->dev, "Submit Rx URB success\n"); */
		ret = 0;
	}

rx_ret:
	return ret;
}

static int if_usb_submit_rx_urb_fwload(struct usb_card_rec *cardp)
{
	return __if_usb_submit_rx_urb(cardp, &if_usb_receive_fwload);
}

static int if_usb_submit_rx_urb(struct usb_card_rec *cardp)
{
	return __if_usb_submit_rx_urb(cardp, &if_usb_receive);
}

static void if_usb_receive_fwload(struct urb *urb)
{
	struct read_cb_info *rinfo = (struct read_cb_info *)urb->context;
	struct sk_buff *skb = rinfo->skb;
	struct usb_card_rec *cardp = (struct usb_card_rec *)rinfo->cardp;
	struct fwsyncheader *syncfwheader;
	struct bootcmdrespStr bootcmdresp;

	if (urb->status) {
		lbs_deb_usbd(&cardp->udev->dev,
			    "URB status is failed during fw load\n");
		kfree_skb(skb);
		return;
	}

	if (cardp->fwdnldover) {
		__le32 *tmp = (__le32 *)(skb->data + IPFIELD_ALIGN_OFFSET);

		if (tmp[0] == cpu_to_le32(CMD_TYPE_INDICATION) &&
		    tmp[1] == cpu_to_le32(MACREG_INT_CODE_FIRMWARE_READY)) {
			lbs_pr_info("Firmware ready event received\n");
			wake_up(&cardp->fw_wq);
		} else {
			lbs_deb_usb("Waiting for confirmation; got %x %x\n", le32_to_cpu(tmp[0]),
				    le32_to_cpu(tmp[1]));
			if_usb_submit_rx_urb_fwload(cardp);
		}
		kfree_skb(skb);
		return;
	}
	if (cardp->bootcmdresp <= 0) {
		memcpy (&bootcmdresp, skb->data + IPFIELD_ALIGN_OFFSET,
			sizeof(bootcmdresp));
		if (le16_to_cpu(cardp->udev->descriptor.bcdDevice) < 0x3106) {
			kfree_skb(skb);
			if_usb_submit_rx_urb_fwload(cardp);
			cardp->bootcmdresp = 1;
			lbs_deb_usbd(&cardp->udev->dev,
				    "Received valid boot command response\n");
			return;
		}
		if (bootcmdresp.u32magicnumber != cpu_to_le32(BOOT_CMD_MAGIC_NUMBER)) {
			if (bootcmdresp.u32magicnumber == cpu_to_le32(CMD_TYPE_REQUEST) ||
			    bootcmdresp.u32magicnumber == cpu_to_le32(CMD_TYPE_DATA) ||
			    bootcmdresp.u32magicnumber == cpu_to_le32(CMD_TYPE_INDICATION)) {
				if (!cardp->bootcmdresp)
					lbs_pr_info("Firmware already seems alive; resetting\n");
				cardp->bootcmdresp = -1;
			} else {
				lbs_pr_info("boot cmd response wrong magic number (0x%x)\n",
					    le32_to_cpu(bootcmdresp.u32magicnumber));
			}
		} else if (bootcmdresp.u8cmd_tag != BOOT_CMD_FW_BY_USB) {
			lbs_pr_info(
				"boot cmd response cmd_tag error (%d)\n",
				bootcmdresp.u8cmd_tag);
		} else if (bootcmdresp.u8result != BOOT_CMD_RESP_OK) {
			lbs_pr_info(
				"boot cmd response result error (%d)\n",
				bootcmdresp.u8result);
		} else {
			cardp->bootcmdresp = 1;
			lbs_deb_usbd(&cardp->udev->dev,
				    "Received valid boot command response\n");
		}
		kfree_skb(skb);
		if_usb_submit_rx_urb_fwload(cardp);
		return;
	}

	syncfwheader = kmalloc(sizeof(struct fwsyncheader), GFP_ATOMIC);
	if (!syncfwheader) {
		lbs_deb_usbd(&cardp->udev->dev, "Failure to allocate syncfwheader\n");
		kfree_skb(skb);
		return;
	}

	memcpy(syncfwheader, skb->data + IPFIELD_ALIGN_OFFSET,
			sizeof(struct fwsyncheader));

	if (!syncfwheader->cmd) {
		/*
		lbs_deb_usbd(&cardp->udev->dev,
			    "FW received Blk with correct CRC\n");
		lbs_deb_usbd(&cardp->udev->dev,
			    "FW received Blk seqnum = %d\n",
		       syncfwheader->seqnum);
		*/
		cardp->CRC_OK = 1;
	} else {
		lbs_deb_usbd(&cardp->udev->dev,
			    "FW received Blk with CRC error\n");
		cardp->CRC_OK = 0;
	}

	kfree_skb(skb);

	/* reschedule timer for 200ms hence */
	mod_timer(&cardp->fw_timeout, jiffies + (HZ/5));

	if (cardp->fwfinalblk) {
		cardp->fwdnldover = 1;
		goto exit;
	}

	if_usb_send_fw_pkt(cardp);

 exit:
	if_usb_submit_rx_urb_fwload(cardp);

	kfree(syncfwheader);

	return;
}

#define MRVDRV_MIN_PKT_LEN	30

static inline void process_cmdtypedata(int recvlength, struct sk_buff *skb,
				       struct usb_card_rec *cardp,
				       struct lbs_private *priv)
{
	if (recvlength > MRVDRV_ETH_RX_PACKET_BUFFER_SIZE +
	    MESSAGE_HEADER_LEN || recvlength < MRVDRV_MIN_PKT_LEN) {
		lbs_deb_usbd(&cardp->udev->dev,
			    "Packet length is Invalid\n");
		kfree_skb(skb);
		return;
	}

	skb_reserve(skb, IPFIELD_ALIGN_OFFSET);
	skb_put(skb, recvlength);
	skb_pull(skb, MESSAGE_HEADER_LEN);
	lbs_process_rxed_packet(priv, skb);
	priv->upld_len = (recvlength - MESSAGE_HEADER_LEN);
}

static inline void process_cmdrequest(int recvlength, u8 *recvbuff,
				      struct sk_buff *skb,
				      struct usb_card_rec *cardp,
				      struct lbs_private *priv)
{
	u8 *cmdbuf;
	if (recvlength > LBS_CMD_BUFFER_SIZE) {
		lbs_deb_usbd(&cardp->udev->dev,
			    "The receive buffer is too large\n");
		kfree_skb(skb);
		return;
	}

	if (!in_interrupt())
		BUG();

	spin_lock(&priv->driver_lock);
	/* take care of cur_cmd = NULL case by reading the
	 * data to clear the interrupt */
	if (!priv->cur_cmd) {
		cmdbuf = priv->upld_buf;
		priv->hisregcpy &= ~MRVDRV_CMD_UPLD_RDY;
	} else
		cmdbuf = (u8 *) priv->cur_cmd->cmdbuf;

	cardp->usb_int_cause |= MRVDRV_CMD_UPLD_RDY;
	priv->upld_len = (recvlength - MESSAGE_HEADER_LEN);
	memcpy(cmdbuf, recvbuff + MESSAGE_HEADER_LEN,
	       priv->upld_len);

	kfree_skb(skb);
	lbs_interrupt(priv);
	spin_unlock(&priv->driver_lock);

	lbs_deb_usbd(&cardp->udev->dev,
		    "Wake up main thread to handle cmd response\n");

	return;
}

/**
 *  @brief This function reads of the packet into the upload buff,
 *  wake up the main thread and initialise the Rx callack.
 *
 *  @param urb		pointer to struct urb
 *  @return 	   	N/A
 */
static void if_usb_receive(struct urb *urb)
{
	struct read_cb_info *rinfo = (struct read_cb_info *)urb->context;
	struct sk_buff *skb = rinfo->skb;
	struct usb_card_rec *cardp = (struct usb_card_rec *) rinfo->cardp;
	struct lbs_private *priv = cardp->priv;

	int recvlength = urb->actual_length;
	u8 *recvbuff = NULL;
	u32 recvtype = 0;

	lbs_deb_enter(LBS_DEB_USB);

	if (recvlength) {
		__le32 tmp;

		if (urb->status) {
			lbs_deb_usbd(&cardp->udev->dev,
				    "URB status is failed\n");
			kfree_skb(skb);
			goto setup_for_next;
		}

		recvbuff = skb->data + IPFIELD_ALIGN_OFFSET;
		memcpy(&tmp, recvbuff, sizeof(u32));
		recvtype = le32_to_cpu(tmp);
		lbs_deb_usbd(&cardp->udev->dev,
			    "Recv length = 0x%x, Recv type = 0x%X\n",
			    recvlength, recvtype);
	} else if (urb->status) {
		kfree_skb(skb);
		goto rx_exit;
	}

	switch (recvtype) {
	case CMD_TYPE_DATA:
		process_cmdtypedata(recvlength, skb, cardp, priv);
		break;

	case CMD_TYPE_REQUEST:
		process_cmdrequest(recvlength, recvbuff, skb, cardp, priv);
		break;

	case CMD_TYPE_INDICATION:
		/* Event cause handling */
		spin_lock(&priv->driver_lock);
		cardp->usb_event_cause = le32_to_cpu(*(__le32 *) (recvbuff + MESSAGE_HEADER_LEN));
		lbs_deb_usbd(&cardp->udev->dev,"**EVENT** 0x%X\n",
			    cardp->usb_event_cause);
		if (cardp->usb_event_cause & 0xffff0000) {
			lbs_send_tx_feedback(priv);
			spin_unlock(&priv->driver_lock);
			break;
		}
		cardp->usb_event_cause <<= 3;
		cardp->usb_int_cause |= MRVDRV_CARDEVENT;
		kfree_skb(skb);
		lbs_interrupt(priv);
		spin_unlock(&priv->driver_lock);
		goto rx_exit;
	default:
		lbs_deb_usbd(&cardp->udev->dev, "Unknown command type 0x%X\n",
		             recvtype);
		kfree_skb(skb);
		break;
	}

setup_for_next:
	if_usb_submit_rx_urb(cardp);
rx_exit:
	lbs_deb_leave(LBS_DEB_USB);
}

/**
 *  @brief This function downloads data to FW
 *  @param priv		pointer to struct lbs_private structure
 *  @param type		type of data
 *  @param buf		pointer to data buffer
 *  @param len		number of bytes
 *  @return 	   	0 or -1
 */
static int if_usb_host_to_card(struct lbs_private *priv,
	u8 type,
	u8 *payload,
	u16 nb)
{
	struct usb_card_rec *cardp = (struct usb_card_rec *)priv->card;

	lbs_deb_usbd(&cardp->udev->dev,"*** type = %u\n", type);
	lbs_deb_usbd(&cardp->udev->dev,"size after = %d\n", nb);

	if (type == MVMS_CMD) {
		__le32 tmp = cpu_to_le32(CMD_TYPE_REQUEST);
		priv->dnld_sent = DNLD_CMD_SENT;
		memcpy(cardp->bulk_out_buffer, (u8 *) & tmp,
		       MESSAGE_HEADER_LEN);

	} else {
		__le32 tmp = cpu_to_le32(CMD_TYPE_DATA);
		priv->dnld_sent = DNLD_DATA_SENT;
		memcpy(cardp->bulk_out_buffer, (u8 *) & tmp,
		       MESSAGE_HEADER_LEN);
	}

	memcpy((cardp->bulk_out_buffer + MESSAGE_HEADER_LEN), payload, nb);

	return usb_tx_block(cardp, cardp->bulk_out_buffer,
	                    nb + MESSAGE_HEADER_LEN);
}

/* called with priv->driver_lock held */
static int if_usb_get_int_status(struct lbs_private *priv, u8 *ireg)
{
	struct usb_card_rec *cardp = priv->card;

	*ireg = cardp->usb_int_cause;
	cardp->usb_int_cause = 0;

	lbs_deb_usbd(&cardp->udev->dev,"Int cause is 0x%X\n", *ireg);

	return 0;
}

static int if_usb_read_event_cause(struct lbs_private *priv)
{
	struct usb_card_rec *cardp = priv->card;

	priv->eventcause = cardp->usb_event_cause;
	/* Re-submit rx urb here to avoid event lost issue */
	if_usb_submit_rx_urb(cardp);
	return 0;
}

/**
 *  @brief This function issues Boot command to the Boot2 code
 *  @param ivalue   1:Boot from FW by USB-Download
 *                  2:Boot from FW in EEPROM
 *  @return 	   	0
 */
static int if_usb_issue_boot_command(struct usb_card_rec *cardp, int ivalue)
{
	struct bootcmdstr sbootcmd;
	int i;

	/* Prepare command */
	sbootcmd.u32magicnumber = cpu_to_le32(BOOT_CMD_MAGIC_NUMBER);
	sbootcmd.u8cmd_tag = ivalue;
	for (i=0; i<11; i++)
		sbootcmd.au8dumy[i]=0x00;
	memcpy(cardp->bulk_out_buffer, &sbootcmd, sizeof(struct bootcmdstr));

	/* Issue command */
	usb_tx_block(cardp, cardp->bulk_out_buffer, sizeof(struct bootcmdstr));

	return 0;
}


/**
 *  @brief This function checks the validity of Boot2/FW image.
 *
 *  @param data              pointer to image
 *         len               image length
 *  @return     0 or -1
 */
static int check_fwfile_format(u8 *data, u32 totlen)
{
	u32 bincmd, exit;
	u32 blksize, offset, len;
	int ret;

	ret = 1;
	exit = len = 0;

	do {
		struct fwheader *fwh = (void *)data;

		bincmd = le32_to_cpu(fwh->dnldcmd);
		blksize = le32_to_cpu(fwh->datalength);
		switch (bincmd) {
		case FW_HAS_DATA_TO_RECV:
			offset = sizeof(struct fwheader) + blksize;
			data += offset;
			len += offset;
			if (len >= totlen)
				exit = 1;
			break;
		case FW_HAS_LAST_BLOCK:
			exit = 1;
			ret = 0;
			break;
		default:
			exit = 1;
			break;
		}
	} while (!exit);

	if (ret)
		lbs_pr_err("firmware file format check FAIL\n");
	else
		lbs_deb_fw("firmware file format check PASS\n");

	return ret;
}


static int if_usb_prog_firmware(struct usb_card_rec *cardp)
{
	int i = 0;
	static int reset_count = 10;
	int ret = 0;

	lbs_deb_enter(LBS_DEB_USB);

	if ((ret = request_firmware(&cardp->fw, lbs_fw_name,
				    &cardp->udev->dev)) < 0) {
		lbs_pr_err("request_firmware() failed with %#x\n", ret);
		lbs_pr_err("firmware %s not found\n", lbs_fw_name);
		goto done;
	}

	if (check_fwfile_format(cardp->fw->data, cardp->fw->size))
		goto release_fw;

restart:
	if (if_usb_submit_rx_urb_fwload(cardp) < 0) {
		lbs_deb_usbd(&cardp->udev->dev, "URB submission is failed\n");
		ret = -1;
		goto release_fw;
	}

	cardp->bootcmdresp = 0;
	do {
		int j = 0;
		i++;
		/* Issue Boot command = 1, Boot from Download-FW */
		if_usb_issue_boot_command(cardp, BOOT_CMD_FW_BY_USB);
		/* wait for command response */
		do {
			j++;
			msleep_interruptible(100);
		} while (cardp->bootcmdresp == 0 && j < 10);
	} while (cardp->bootcmdresp == 0 && i < 5);

	if (cardp->bootcmdresp <= 0) {
		if (--reset_count >= 0) {
			if_usb_reset_device(cardp);
			goto restart;
		}
		return -1;
	}

	i = 0;

	cardp->totalbytes = 0;
	cardp->fwlastblksent = 0;
	cardp->CRC_OK = 1;
	cardp->fwdnldover = 0;
	cardp->fwseqnum = -1;
	cardp->totalbytes = 0;
	cardp->fwfinalblk = 0;

	/* Send the first firmware packet... */
	if_usb_send_fw_pkt(cardp);

	/* ... and wait for the process to complete */
	wait_event_interruptible(cardp->fw_wq, cardp->surprise_removed || cardp->fwdnldover);
	
	del_timer_sync(&cardp->fw_timeout);
	usb_kill_urb(cardp->rx_urb);

	if (!cardp->fwdnldover) {
		lbs_pr_info("failed to load fw, resetting device!\n");
		if (--reset_count >= 0) {
			if_usb_reset_device(cardp);
			goto restart;
		}

		lbs_pr_info("FW download failure, time = %d ms\n", i * 100);
		ret = -1;
		goto release_fw;
	}

release_fw:
	release_firmware(cardp->fw);
	cardp->fw = NULL;

done:
	lbs_deb_leave_args(LBS_DEB_USB, "ret %d", ret);
	return ret;
}


#ifdef CONFIG_PM
static int if_usb_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct usb_card_rec *cardp = usb_get_intfdata(intf);
	struct lbs_private *priv = cardp->priv;

	lbs_deb_enter(LBS_DEB_USB);

	if (priv->psstate != PS_STATE_FULL_POWER)
		return -1;

	netif_device_detach(priv->dev);
	netif_device_detach(priv->mesh_dev);

	/* Unlink tx & rx urb */
	usb_kill_urb(cardp->tx_urb);
	usb_kill_urb(cardp->rx_urb);

	lbs_deb_leave(LBS_DEB_USB);
	return 0;
}

static int if_usb_resume(struct usb_interface *intf)
{
	struct usb_card_rec *cardp = usb_get_intfdata(intf);
	struct lbs_private *priv = cardp->priv;

	lbs_deb_enter(LBS_DEB_USB);

	if_usb_submit_rx_urb(cardp);

	netif_device_attach(priv->dev);
	netif_device_attach(priv->mesh_dev);

	lbs_deb_leave(LBS_DEB_USB);
	return 0;
}
#else
#define if_usb_suspend NULL
#define if_usb_resume NULL
#endif

static struct usb_driver if_usb_driver = {
	.name = DRV_NAME,
	.probe = if_usb_probe,
	.disconnect = if_usb_disconnect,
	.id_table = if_usb_table,
	.suspend = if_usb_suspend,
	.resume = if_usb_resume,
};

static int __init if_usb_init_module(void)
{
	int ret = 0;

	lbs_deb_enter(LBS_DEB_MAIN);

	ret = usb_register(&if_usb_driver);

	lbs_deb_leave_args(LBS_DEB_MAIN, "ret %d", ret);
	return ret;
}

static void __exit if_usb_exit_module(void)
{
	lbs_deb_enter(LBS_DEB_MAIN);

	usb_deregister(&if_usb_driver);

	lbs_deb_leave(LBS_DEB_MAIN);
}

module_init(if_usb_init_module);
module_exit(if_usb_exit_module);

MODULE_DESCRIPTION("8388 USB WLAN Driver");
MODULE_AUTHOR("Marvell International Ltd.");
MODULE_LICENSE("GPL");
