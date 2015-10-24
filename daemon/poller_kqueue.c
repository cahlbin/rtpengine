#include "poller.h"

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <errno.h>
#include <sys/event.h>
#include <glib.h>
#include <sys/time.h>

#include "aux.h"
#include "obj.h"




struct timer_item {
	struct obj			obj;
	void				(*func)(void *);
	struct obj			*obj_ptr;
};

struct poller_item_int {
	struct obj			obj;
	struct poller_item		item;

	int				blocked:1;
	int				error:1;
};

struct poller {
	int				fd;
	mutex_t				lock;
	struct poller_item_int		**items;
	unsigned int			items_size;

	mutex_t				timers_lock;
	GSList				*timers;
	mutex_t				timers_add_del_lock; /* nested below timers_lock */
	GSList				*timers_add;
	GSList				*timers_del;
};





struct poller *poller_new(void) {
	struct poller *p;

	p = malloc(sizeof(*p));
	memset(p, 0, sizeof(*p));
	gettimeofday(&g_now, NULL);
	p->fd = kqueue();
	if (p->fd == -1)
		abort();
	mutex_init(&p->lock);
	mutex_init(&p->timers_lock);
	mutex_init(&p->timers_add_del_lock);

	return p;
}


static int kqueue_filter(struct poller_item *it, struct poller_item_int *ii) {
	if (!it)
		it = &ii->item;
	return ((it->writeable && ii && ii->blocked) ? EVFILT_WRITE : 0) |
		(it->readable ? EVFILT_READ : 0);
}


static void poller_fd_timer(void *p) {
	struct poller_item_int *it = p;

	if (it->item.timer)
		it->item.timer(it->item.fd, it->item.obj, it->item.uintp);
}


static void poller_item_free(void *p) {
	struct poller_item_int *i = p;
	obj_put_o(i->item.obj);
}


/* unlocks on return */
static int __poller_add_item(struct poller *p, struct poller_item *i, int has_lock) {
	struct poller_item_int *ip;
	unsigned int u;
    struct kevent e;

	if (!p || !i)
		goto fail_lock;
	if (i->fd < 0)
		goto fail_lock;
	if (!i->readable && !i->writeable)
		goto fail_lock;
	if (!i->closed)
		goto fail_lock;

	if (!has_lock)
		mutex_lock(&p->lock);

	if (i->fd < p->items_size && p->items[i->fd])
		goto fail;

	ZERO(e);
    int kfilter = kqueue_filter(i, NULL);
    EV_SET(&e, i->fd, kfilter, EV_ADD | EV_CLEAR, 0, 0, NULL);
	if (kevent(p->fd, &e, 1, NULL, 0, NULL))
		abort();

	if (i->fd >= p->items_size) {
		u = p->items_size;
		p->items_size = i->fd + 1;
		p->items = realloc(p->items, sizeof(*p->items) * p->items_size);
		memset(p->items + u, 0, sizeof(*p->items) * (p->items_size - u - 1));
	}

	ip = obj_alloc0("poller_item_int", sizeof(*ip), poller_item_free);
	memcpy(&ip->item, i, sizeof(*i));
	obj_hold_o(ip->item.obj); /* new ref in *ip */
	p->items[i->fd] = obj_get(ip);

	mutex_unlock(&p->lock);

	if (i->timer)
		poller_add_timer(p, poller_fd_timer, &ip->obj);

	obj_put(ip);

	return 0;

fail:
	mutex_unlock(&p->lock);
	return -1;
fail_lock:
	if (has_lock)
		mutex_unlock(&p->lock);
	return -1;
}


int poller_add_item(struct poller *p, struct poller_item *i) {
	return __poller_add_item(p, i, 0);
}


int poller_del_item(struct poller *p, int fd) {
	struct poller_item_int *it;

	if (!p || fd < 0)
		return -1;

	mutex_lock(&p->lock);

	if (fd >= p->items_size)
		goto fail;
	if (!p->items || !(it = p->items[fd]))
		goto fail;

    int kfilter = kqueue_filter(NULL, it);
    struct kevent e;
    EV_SET(&e, fd, kfilter, EV_DELETE, 0, 0, NULL);
	if (kevent(p->fd, &e, 1, NULL, 0, NULL))
		abort();

	p->items[fd] = NULL; /* stealing the ref */

	mutex_unlock(&p->lock);

	if (it->item.timer)
		poller_del_timer(p, poller_fd_timer, &it->obj);

	obj_put(it);

	return 0;

fail:
	mutex_unlock(&p->lock);
	return -1;
}


int poller_update_item(struct poller *p, struct poller_item *i) {
	struct poller_item_int *np;

	if (!p || !i)
		return -1;
	if (i->fd < 0)
		return -1;
	if (!i->readable && !i->writeable)
		return -1;
	if (!i->closed)
		return -1;

	mutex_lock(&p->lock);

	if (i->fd >= p->items_size || !(np = p->items[i->fd]))
		return __poller_add_item(p, i, 1);

	obj_hold_o(i->obj);
	obj_put_o(np->item.obj);
	np->item.obj = i->obj;
	np->item.uintp = i->uintp;
	np->item.readable = i->readable;
	np->item.writeable = i->writeable;
	np->item.closed = i->closed;
	/* updating timer is not supported */

	mutex_unlock(&p->lock);

	return 0;
}


/* timers_lock and timers_add_del_lock must be held */
static void poller_timers_mod(struct poller *p) {
	GSList *l, **ll, **kk;
	struct timer_item *ti, *tj;

	ll = &p->timers_add;
	while (*ll) {
		l = *ll;
		*ll = l->next;
		l->next = p->timers;
		p->timers = l;
	}

	ll = &p->timers_del;
	while (*ll) {
		ti = (*ll)->data;
		kk = &p->timers;
		while (*kk) {
			tj = (*kk)->data;
			if (tj->func != ti->func)
				goto next;
			if (tj->obj_ptr != ti->obj_ptr)
				goto next;
			goto found;
next:
			kk = &(*kk)->next;
		}
		/* deleted a timer that wasn't added yet. possible race, otherwise bug */
		ll = &(*ll)->next;
		continue;
found:
		l = *ll;
		*ll = (*ll)->next;
		obj_put_o(l->data);
		g_slist_free_1(l);

		l = *kk;
		*kk = (*kk)->next;
		obj_put_o(l->data);
		g_slist_free_1(l);
	}
}


static void poller_timers_run(struct poller *p) {
	GSList *l;
	struct timer_item *ti;

	mutex_lock(&p->timers_lock);
	mutex_lock(&p->timers_add_del_lock);
	poller_timers_mod(p);
	mutex_unlock(&p->timers_add_del_lock);

	for (l = p->timers; l; l = l->next) {
		ti = l->data;
		ti->func(ti->obj_ptr);
	}

	mutex_lock(&p->timers_add_del_lock);
	poller_timers_mod(p);
	mutex_unlock(&p->timers_add_del_lock);
	mutex_unlock(&p->timers_lock);
}

int poller_poll(struct poller *p, int timeout_ms) {
	int ret, i;
	struct poller_item_int *it;
    struct kevent evs[128], *ev, e;
    struct timespec timeout = { 0, timeout_ms * 1000000 };

	if (!p)
		return -1;

	mutex_lock(&p->lock);

	ret = -1;
	if (!p->items || !p->items_size)
		goto out;

	mutex_unlock(&p->lock);
	errno = 0;
    ret = kevent(p->fd, NULL, 0, &evs[0], sizeof(evs) / sizeof(*evs), &timeout);
	mutex_lock(&p->lock);

	if (errno == EINTR)
		ret = 0;
	if (ret == 0)
		ret = 0;
	if (ret <= 0)
		goto out;

	gettimeofday(&g_now, NULL);

	for (i = 0; i < ret; i++) {
		ev = &evs[i];

		if ((int)(ev->ident) < 0)
			continue;

		it = (ev->ident < p->items_size) ? p->items[ev->ident] : NULL;
		if (!it)
			continue;

		obj_hold(it);
		mutex_unlock(&p->lock);

		if (it->error) {
			it->item.closed(it->item.fd, it->item.obj, it->item.uintp);
			goto next;
		}

		if ((ev->flags & EV_EOF) || (ev->flags & EV_ERROR))
			it->item.closed(it->item.fd, it->item.obj, it->item.uintp);
		else if (ev->filter == EVFILT_WRITE) {
			mutex_lock(&p->lock);
			it->blocked = 0;
			mutex_unlock(&p->lock);
			it->item.writeable(it->item.fd, it->item.obj, it->item.uintp);
		}
		else if (ev->filter == EVFILT_READ)
			it->item.readable(it->item.fd, it->item.obj, it->item.uintp);
		else if (!ev->filter)
			goto next;
		else
			abort();

next:
		obj_put(it);
		mutex_lock(&p->lock);
	}


out:
	mutex_unlock(&p->lock);
	return ret;
}


void poller_blocked(struct poller *p, int fd) {
	if (!p || fd < 0)
		return;

	mutex_lock(&p->lock);

	if (fd >= p->items_size)
		goto fail;
	if (!p->items || !p->items[fd])
		goto fail;
	if (!p->items[fd]->item.writeable)
		goto fail;

	p->items[fd]->blocked = 1;

fail:
	mutex_unlock(&p->lock);
}

void poller_error(struct poller *p, int fd) {
	if (!p || fd < 0)
		return;

	mutex_lock(&p->lock);

	if (fd >= p->items_size)
		goto fail;
	if (!p->items || !p->items[fd])
		goto fail;
	if (!p->items[fd]->item.writeable)
		goto fail;

	p->items[fd]->error = 1;
	p->items[fd]->blocked = 1;

fail:
	mutex_unlock(&p->lock);
}

int poller_isblocked(struct poller *p, int fd) {
	int ret;

	if (!p || fd < 0)
		return -1;

	mutex_lock(&p->lock);

	ret = -1;
	if (fd >= p->items_size)
		goto out;
	if (!p->items || !p->items[fd])
		goto out;
	if (!p->items[fd]->item.writeable)
		goto out;

	ret = p->items[fd]->blocked ? 1 : 0;

out:
	mutex_unlock(&p->lock);
	return ret;
}



static void timer_item_free(void *p) {
	struct timer_item *i = p;
	if (i->obj_ptr)
		obj_put_o(i->obj_ptr);
}

static int poller_timer_link(struct poller *p, GSList **lp, void (*f)(void *), struct obj *o) {
	struct timer_item *i;

	if (!f)
		return -1;

	i = obj_alloc0("timer_item", sizeof(*i), timer_item_free);

	i->func = f;
	i->obj_ptr = o ? obj_hold_o(o) : NULL;

	mutex_lock(&p->timers_add_del_lock);
	*lp = g_slist_prepend(*lp, i);

	if (!mutex_trylock(&p->timers_lock)) {
		poller_timers_mod(p);
		mutex_unlock(&p->timers_lock);
	}

	mutex_unlock(&p->timers_add_del_lock);

	return 0;
}

int poller_del_timer(struct poller *p, void (*f)(void *), struct obj *o) {
	return poller_timer_link(p, &p->timers_del, f, o);
}

int poller_add_timer(struct poller *p, void (*f)(void *), struct obj *o) {
	return poller_timer_link(p, &p->timers_add, f, o);
}

/* run in thread separate from poller_poll() */
void poller_timer_loop(void *d) {
	struct poller *p = d;
	struct timeval tv;
	int wt;

	while (!g_shutdown) {
		gettimeofday(&tv, NULL);
		if (tv.tv_sec != poller_now)
			goto now;

		wt = 1000000 - tv.tv_usec;
		wt = MIN(wt, 100000);
		usleep(wt);
		continue;

now:
		gettimeofday(&g_now, NULL);
		poller_timers_run(p);
	}
}

void poller_loop(void *d) {
	struct poller *p = d;

	while (!g_shutdown)
		poller_poll(p, 100);
}
