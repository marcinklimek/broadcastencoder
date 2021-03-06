/*****************************************************************************
 * udp.c : UDP output functions
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
 *
 * Large Portions of this code originate from FFmpeg
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 *****************************************************************************/

#include "common/common.h"
#include "common/network/network.h"
#include "output/output.h"
#include "udp.h"

typedef struct
{
    int udp_fd;
    int ttl;
    int buffer_size;
    int is_multicast;
    int local_port;
    int reuse_socket;
    struct sockaddr_storage dest_addr;
    int dest_addr_len;
    int is_connected;
    int max_packet_size;
    int miface;
} obe_udp_ctx;

static int udp_set_multicast_opts( int sockfd, obe_udp_ctx *s )
{
    struct sockaddr *addr = (struct sockaddr *)&s->dest_addr;

    if( addr->sa_family == AF_INET )
    {
#ifdef IP_MULTICAST_TTL
        if( setsockopt( sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &s->ttl, sizeof(s->ttl) ) < 0 )
        {
            fprintf( stderr, "[udp] Could not setup IPv4 multicast\n" );
            return -1;
        }
#endif

#ifdef IP_MULTICAST_IF
        struct ip_mreqn req = { .imr_ifindex = s->miface };
        if( setsockopt( sockfd, IPPROTO_IP, IP_MULTICAST_IF, &req, sizeof(req) ) < 0 )
        {
            fprintf( stderr, "[udp] Could not setup multicast interface\n" );
            return -1;
        }
#endif
    }

#ifdef IPPROTO_IPV6
    if( addr->sa_family == AF_INET6 )
    {
#ifdef IPV6_MULTICAST_HOPS
        if( setsockopt( sockfd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &s->ttl, sizeof(s->ttl) ) < 0 )
        {
            fprintf( stderr, "[udp] Could not setup IPv6 multicast\n" );
            return -1;
        }
#endif
#ifdef IPV6_MULTICAST_IF
        if( setsockopt( sockfd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &s->miface, sizeof(s->miface) ) < 0 )
        {
            fprintf( stderr, "[udp] Could not setup IPv6 multicast interface\n" );
            return -1;
        }
#endif
    }
#endif
    return 0;
}

static struct addrinfo* udp_resolve_host( const char *hostname, int port, int type, int family, int flags )
{
    struct addrinfo hints, *res = 0;
    int error;
    char sport[16];
    const char *node = 0, *service = "0";

    if( port > 0 )
    {
        snprintf( sport, sizeof(sport), "%d", port );
        service = sport;
    }
    if( (hostname) && (hostname[0] != '\0') && (hostname[0] != '?') )
        node = hostname;

    memset( &hints, 0, sizeof(hints) );
    hints.ai_socktype = type;
    hints.ai_family   = family;
    hints.ai_flags    = flags;
    if( (error = getaddrinfo( node, service, &hints, &res )) )
    {
        res = NULL;
        fprintf( stderr, "[udp] error: %s \n", gai_strerror( error ) );
    }

    return res;
}

static int udp_set_url( struct sockaddr_storage *addr, const char *hostname, int port )
{
    struct addrinfo *res0;
    int addr_len;

    res0 = udp_resolve_host( hostname, port, SOCK_DGRAM, AF_UNSPEC, 0 );
    if( res0 == 0 )
        return -1;
    memcpy( addr, res0->ai_addr, res0->ai_addrlen );
    addr_len = res0->ai_addrlen;
    freeaddrinfo( res0 );

    return addr_len;
}

static int udp_socket_create( obe_udp_ctx *s, struct sockaddr_storage *addr, int *addr_len )
{
    int udp_fd = -1;
    struct addrinfo *res0 = NULL, *res = NULL;
    int family = AF_UNSPEC;

    if( ((struct sockaddr *) &s->dest_addr)->sa_family )
        family = ((struct sockaddr *) &s->dest_addr)->sa_family;
    res0 = udp_resolve_host( 0, s->local_port, SOCK_DGRAM, family, AI_PASSIVE );
    if( res0 == 0 )
        goto fail;
    for( res = res0; res; res=res->ai_next )
    {
        udp_fd = socket( res->ai_family, SOCK_DGRAM, 0 );
        if( udp_fd > 0 )
            break;
        // TODO error
    }

    if( udp_fd < 0 )
        goto fail;

    memcpy( addr, res->ai_addr, res->ai_addrlen );
    *addr_len = res->ai_addrlen;

    freeaddrinfo( res0 );

    return udp_fd;

 fail:
    if( udp_fd >= 0 )
        close( udp_fd );
    if( res0 )
        freeaddrinfo( res0 );
    return -1;
}

static int udp_port( struct sockaddr_storage *addr, int addr_len )
{
    char sbuf[sizeof(int)*3+1];

    if( getnameinfo( (struct sockaddr *)addr, addr_len, NULL, 0, sbuf, sizeof(sbuf), NI_NUMERICSERV ) != 0 )
    {
        fprintf( stderr, "[udp]: getnameinfo failed \n" );
        return -1;
    }

    return strtol( sbuf, NULL, 10 );
}

/**
 * If no filename is given to av_open_input_file because you want to
 * get the local port first, then you must call this function to set
 * the remote server address.
 *
 * url syntax: udp://host:port[?option=val...]
 * option: 'ttl=n'       : set the ttl value (for multicast only)
 *         'localport=n' : set the local port
 *         'pkt_size=n'  : set max packet size
 *         'reuse=1'     : enable reusing the socket
 *
 * @param h media file context
 * @param uri of the remote server
 * @return zero if no error.
 */
static int udp_set_remote_url( obe_udp_ctx *s, const char *uri )
{
    char hostname[256], buf[10];
    int port;
    const char *p;

    av_url_split( NULL, 0, NULL, 0, hostname, sizeof(hostname), &port, NULL, 0, uri );

    /* set the destination address */
    s->dest_addr_len = udp_set_url(&s->dest_addr, hostname, port);
    if( s->dest_addr_len < 0 )
        return -1;

    s->is_multicast = is_multicast_address( (struct sockaddr*) &s->dest_addr );
    p = strchr(uri, '?');
    if( p )
    {
        if( av_find_info_tag( buf, sizeof(buf), "connect", p ) )
        {
            int was_connected = s->is_connected;
            s->is_connected = strtol( buf, NULL, 10 );
            if( s->is_connected && !was_connected )
            {
                if( connect( s->udp_fd, (struct sockaddr *) &s->dest_addr, s->dest_addr_len ) )
                {
                    s->is_connected = 0;
                    fprintf( stderr, "[udp]: connect() failed: \n" );
                    return -1;
                }
            }
        }
    }

    return 0;
}

int udp_open( hnd_t *p_handle, char *uri )
{
    char hostname[1024];
    int port, udp_fd = -1, tmp, bind_ret = -1;
    const char *p;
    char buf[256];
    struct sockaddr_storage my_addr;
    int len;
    int reuse_specified = 0;

    obe_udp_ctx *s = calloc( 1, sizeof(*s) );
    *p_handle = NULL;
    if( !s )
        return -1;

    p = strchr( uri, '?' );
    if( p )
    {
        if( av_find_info_tag( buf, sizeof(buf), "reuse", p ) )
        {
            const char *endptr = NULL;
            s->reuse_socket = strtol( buf, (char **)&endptr, 10 );
            /* assume if no digits were found it is a request to enable it */
            if( buf == endptr )
                s->reuse_socket = 1;
            reuse_specified = 1;
        }
        if( av_find_info_tag( buf, sizeof(buf), "ttl", p ) )
            s->ttl = strtol( buf, NULL, 10 );

        if( av_find_info_tag( buf, sizeof(buf), "localport", p ) )
            s->local_port = strtol( buf, NULL, 10 );

        if( av_find_info_tag( buf, sizeof(buf), "pkt_size", p ) )
            s->max_packet_size = strtol( buf, NULL, 10 );

        if( av_find_info_tag( buf, sizeof(buf), "buffer_size", p ) )
            s->buffer_size = strtol( buf, NULL, 10 );

        if( av_find_info_tag( buf, sizeof(buf), "connect", p ) )
            s->is_connected = strtol( buf, NULL, 10 );

        if( av_find_info_tag( buf, sizeof(buf), "miface", p ) )
            s->miface = if_nametoindex( buf );
    }

    /* fill the dest addr */
    av_url_split( NULL, 0, NULL, 0, hostname, sizeof(hostname), &port, NULL, 0, uri );

    if( udp_set_remote_url( s, uri ) < 0 )
        goto fail;

    udp_fd = udp_socket_create( s, &my_addr, &len );
    if( udp_fd < 0 )
        goto fail;

    /* Follow the requested reuse option, unless it's multicast in which
     * case enable reuse unless explicitely disabled.
     */
    if( s->reuse_socket || (s->is_multicast && !reuse_specified) )
    {
        s->reuse_socket = 1;
        if( setsockopt( udp_fd, SOL_SOCKET, SO_REUSEADDR, &(s->reuse_socket), sizeof(s->reuse_socket) ) != 0)
            goto fail;
    }

    /* bind to the local address if not multicast or if the multicast
     * bind failed */
    if( bind_ret < 0 && bind( udp_fd, (struct sockaddr *)&my_addr, len ) < 0 )
        goto fail;

    len = sizeof(my_addr);
    getsockname( udp_fd, (struct sockaddr *)&my_addr, (socklen_t *) &len );
    s->local_port = udp_port( &my_addr, len );

    /* set output multicast ttl */
    if( s->is_multicast && udp_set_multicast_opts( udp_fd, s ) < 0 )
        goto fail;

    /* limit the tx buf size to limit latency */
    tmp = s->buffer_size;
    if( setsockopt( udp_fd, SOL_SOCKET, SO_SNDBUF, &tmp, sizeof(tmp) ) < 0 )
        goto fail;

    if( s->is_connected && connect( udp_fd, (struct sockaddr *)&s->dest_addr, s->dest_addr_len ) )
        goto fail;

    s->udp_fd = udp_fd;
    *p_handle = s;
    return 0;

 fail:
    if( udp_fd >= 0 )
        close( udp_fd );

    free( s );
    return -1;
}

int udp_write( hnd_t handle, uint8_t *buf, int size )
{
    obe_udp_ctx *s = handle;
    int ret;

    if( !s->is_connected )
        ret = sendto( s->udp_fd, buf, size, 0, (struct sockaddr *)&s->dest_addr, s->dest_addr_len );
    else
        ret = send( s->udp_fd, buf, size, 0 );

    if( ret < 0 )
    {
        syslog( LOG_WARNING, "UDP packet failed to send\n" );
        return -1;
    }

    return size;
}

void udp_close( hnd_t handle )
{
    obe_udp_ctx *s = handle;

    close( s->udp_fd );
    free( s );
}
