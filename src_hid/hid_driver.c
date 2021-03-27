/**************************************************************************//**
 * @file     hid_driver.c
 * @version  V1.00
 * $Revision: 11 $
 * $Date: 14/12/02 5:47p $
 * @brief    N9H30 MCU USB Host HID driver
 *
 * @note
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2016 Nuvoton Technology Corp. All rights reserved.
*****************************************************************************/

#include <stdio.h>
#include <string.h>

#include "N9H30.h"

#include "usb.h"
#include "usbh_lib.h"
#include "usbh_hid.h"


/// @cond HIDDEN_SYMBOLS

static HID_DEV_T  g_hid_dev[CONFIG_HID_MAX_DEV];

static HID_DEV_T *g_hdev_list = NULL;

static HID_CONN_FUNC *g_hid_conn_func, *g_hid_disconn_func;

static HID_DEV_T *alloc_hid_device(void)
{
    int     i;

    for (i = 0; i < CONFIG_HID_MAX_DEV; i++)
    {
        if (g_hid_dev[i].iface == NULL)
        {
            memset((char *)&g_hid_dev[i], 0, sizeof(HID_DEV_T));
            g_hid_dev[i].uid = get_ticks();
            return &g_hid_dev[i];
        }
    }
    return NULL;
}

void  free_hid_device(HID_DEV_T *hid_dev)
{
    hid_dev->iface = NULL;
    memset((char *)hid_dev, 0, sizeof(HID_DEV_T));
}

static void hid_write_callback(UTR_T *utr)
{
    if (utr)
        HID_DBGMSG("Write to ep %02 complete\n", utr->ep->bEndpointAddress);
}

static int hid_probe(IFACE_T *iface)
{
    UDEV_T       *udev = iface->udev;
    ALT_IFACE_T  *aif = iface->aif;
    DESC_IF_T    *ifd;
    EP_INFO_T    *ep = NULL;
    HID_DEV_T    *hdev, *p;
    int          i;

    ifd = aif->ifd;

    hidtype_t type = UNKNOWN;
    /* Is this a generic interface HID class? */
    if (ifd->bInterfaceClass == USB_CLASS_HID)
        type = GENERIC;
    /* It is a vendor specific input device */
    //FIXME: Are these true across all devices including 3rd party?
    else if (ifd->bInterfaceSubClass == 0x5D && //Xbox360 bInterfaceSubClass
             ifd->bInterfaceProtocol == 0x01)   //Xbox360 bInterfaceProtocol
        type = XBOX360_WIRED;
    else if (ifd->bInterfaceSubClass == 0x5D && //Xbox360 wireless bInterfaceSubClass
             ifd->bInterfaceProtocol == 0x81)   //Xbox360 wireless bInterfaceProtocol
        type = XBOX360_WIRELESS;
    else if (ifd->bInterfaceSubClass == 0x47 && //Xbone and SX bInterfaceSubClass
             ifd->bInterfaceProtocol == 0xD0 && //Xbone and SX bInterfaceProtocol
             aif->ep[0].bInterval == 0x04 &&    //These controllers have multiple interfaces with the same class.
             aif->ep[1].bInterval == 0x04)      //The one we want has a bInternal of 4ms on both eps.
        type = XBOXONE;
    else if (ifd->bInterfaceClass == 0x58 &&  //Xbox OG Device bInterfaceClass
             ifd->bInterfaceSubClass == 0x42) //Xbox OG Device bInterfaceSubClass
    {
        //Device is an OG Xbox peripheral. Check the XID descriptor to find out what type:
        //Ref https://xboxdevwiki.net/Xbox_Input_Devices
        uint8_t *xid_buff = usbh_alloc_mem(32);
        uint32_t xfer_len;
        usbh_ctrl_xfer(iface->udev,
                             0xC1,          /* bmRequestType */
                             0x06,          /* bRequest */
                             0x4200,        /* wValue */
                             iface->if_num, /* wIndex */
                             32,            /* wLength */
                             xid_buff, &xfer_len, 100);
        uint8_t xid_bType = xid_buff[4];
        usbh_free_mem(xid_buff, 32);
        switch (xid_bType)
        {
            case 0x01: type = XBOXOG_CONTROLLER;     break; //Duke,S,Wheel,Arcade stick
            case 0x03: type = XBOXOG_XIR;            break; //Xbox DVD Movie Playback IR Dongle
            case 0x80: type = XBOXOG_STEELBATTALION; break; //Steel Battalion Controller
            default:
                HID_DBGMSG("Unknown OG Xbox Peripheral\n");
                return USBH_ERR_NOT_MATCHED;
        }
        HID_DBGMSG("OG Xbox peripheral type %02x connected\n", xid_bType);
    }
    else
        return USBH_ERR_NOT_MATCHED;

    HID_DBGMSG("hid_probe - device (vid=0x%x, pid=0x%x), interface %d.\n",
               udev->descriptor.idVendor, udev->descriptor.idProduct, ifd->bInterfaceNumber);

    /*
     *  Try to find any interrupt endpoints
     */
    for (i = 0; i < aif->ifd->bNumEndpoints; i++)
    {
        if ((aif->ep[i].bmAttributes & EP_ATTR_TT_MASK) == EP_ATTR_TT_INT)
        {
            ep = &aif->ep[i];
            break;
        }
    }

    if (ep == NULL)
        return USBH_ERR_NOT_MATCHED;   // No endpoints, Ignore this interface

    hdev = alloc_hid_device();
    if (hdev == NULL)
        return HID_RET_OUT_OF_MEMORY;

    hdev->iface = iface;
    hdev->idVendor  = udev->descriptor.idVendor;
    hdev->idProduct = udev->descriptor.idProduct;
    hdev->bSubClassCode = ifd->bInterfaceSubClass;
    hdev->bProtocolCode = ifd->bInterfaceProtocol;
    hdev->next = NULL;
    hdev->type = type;
    hdev->user_data = NULL;
    iface->context = (void *)hdev;

    /* Handle controller specific initialisations */
    if (hdev->type == XBOXONE)
    {
        uint8_t xboxone_start_input[] = {0x05, 0x20, 0x00, 0x01, 0x00};
        uint8_t xboxone_s_init[] = {0x05, 0x20, 0x00, 0x0f, 0x06};
        usbh_hid_int_write(hdev, 0, xboxone_start_input, sizeof(xboxone_start_input), hid_write_callback);
        usbh_hid_int_write(hdev, 0, xboxone_s_init, sizeof(xboxone_s_init), hid_write_callback);
    }

    else if (hdev->type == XBOX360_WIRED)
    {
        uint8_t port = iface->udev->port_num;
        port += (port <= 2) ? 2 : -2; //Xbox ports are numbered 3,4,1,2
        uint8_t xbox360wired_set_led[] = {0x01, 0x03, port + 1};
        usbh_hid_int_write(hdev, 0, xbox360wired_set_led, sizeof(xbox360wired_set_led), hid_write_callback);
    }

    else if (hdev->type == XBOX360_WIRELESS)
    {
        uint8_t xbox360w_inquire_present[] = {0x08, 0x00, 0x0F, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        uint8_t xbox360w_set_led[] = {0x00, 0x00, 0x08, 0x40};
        usbh_hid_int_write(hdev, 0, xbox360w_set_led, sizeof(xbox360w_set_led), hid_write_callback);
        usbh_hid_int_write(hdev, 0, xbox360w_inquire_present, sizeof(xbox360w_inquire_present), hid_write_callback);
    }

    /*
     *  Chaining newly found HID device to end of HID device list.
     */
    if (g_hdev_list == NULL)
        g_hdev_list = hdev;
    else
    {
        for (p = g_hdev_list; p->next != NULL; p = p->next)
            ;
        p->next = hdev;
    }

    if (g_hid_conn_func)
        g_hid_conn_func(hdev, 0);

    HID_DBGMSG("usbhid_probe OK.\n");

    return 0;
}


static void  hid_disconnect(IFACE_T *iface)
{
    HID_DEV_T   *hdev, *p;
    UTR_T       *utr;
    int         i;

    hdev = (HID_DEV_T *)(iface->context);

    for (i = 0; i < iface->aif->ifd->bNumEndpoints; i++)
    {
        iface->udev->hc_driver->quit_xfer(NULL, &(iface->aif->ep[i]));
    }

    /*
     *  Abort all UTR of this HID device (interface)
     */
    for (i = 0; i < CONFIG_HID_DEV_MAX_PIPE; i++)
    {
        utr = hdev->utr_list[i];
        if (utr != NULL)
        {
            usbh_quit_utr(utr);             /* Quit the UTR                               */
            usbh_free_mem(utr->buff, utr->ep->wMaxPacketSize);
            free_utr(utr);
        }
    }

    /*
     *  remove it from HID device list
     */
    for (i = 0; i < CONFIG_HID_MAX_DEV; i++)
    {
        if (g_hid_dev[i].iface == iface)
        {
            hdev = &g_hid_dev[i];

            if (hdev == g_hdev_list)
            {
                g_hdev_list = g_hdev_list->next;
            }
            else
            {
                for (p = g_hdev_list; p != NULL; p = p->next)
                {
                    if (p->next == hdev)
                    {
                        p->next = hdev->next;
                        break;
                    }
                }
            }
            HID_DBGMSG("hid_disconnect - device (vid=0x%x, pid=0x%x), interface %d.\n",
                       hdev->idVendor, hdev->idProduct, iface->if_num);

            if (g_hid_disconn_func)
                g_hid_disconn_func(hdev, 0);

            free_hid_device(hdev);
        }
    }
}

/**
  * @brief    Install hdev connect and disconnect callback function.
  *
  * @param[in]  conn_func       hdev connect callback function.
  * @param[in]  disconn_func    hdev disconnect callback function.
  * @return     None.
  */
void usbh_install_hid_conn_callback(HID_CONN_FUNC *conn_func, HID_CONN_FUNC *disconn_func)
{
    g_hid_conn_func = conn_func;
    g_hid_disconn_func = disconn_func;
}


UDEV_DRV_T  hid_driver =
{
    hid_probe,
    hid_disconnect,
    NULL,                       /* suspend */
    NULL,                       /* resume */
};


/// @endcond /* HIDDEN_SYMBOLS */


/**
  * @brief    Initialize USB Host HID driver.
  * @return   None
  */
void usbh_hid_init(void)
{
    memset((char *)&g_hid_dev[0], 0, sizeof(g_hid_dev));
    g_hdev_list = NULL;
    g_hid_conn_func = NULL;
    g_hid_disconn_func = NULL;
    usbh_register_driver(&hid_driver);
}


/**
 *  @brief   Get a list of currently connected USB Hid devices.
 *  @return  A list HID_DEV_T pointer reference to connected HID devices.
 *  @retval  NULL       There's no HID device found.
 *  @retval  Otherwise  A list of connected HID devices.
 *
 *  The HID devices are chained by the "next" member of HID_DEV_T.
 */
HID_DEV_T * usbh_hid_get_device_list(void)
{
    return g_hdev_list;
}


/*** (C) COPYRIGHT 2016 Nuvoton Technology Corp. ***/

