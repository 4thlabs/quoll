# Dashboard: a list, a detail and a chart

A screen of an application, over a small products API: a list to filter, the detail of the product chosen to edit, and
its sales in a [chart.js](https://www.chartjs.org/) chart, a JavaScript widget of the page.

```
dashboard/
├── api/        # the contract: product.hpp, products_api.hpp
├── server/     # serves it over HTTP: service.hpp handles each endpoint, the products in memory
├── web/        # the page, in C++ compiled to WASM, and the chart's module
└── tests/      # the service through a REST client, and the page on memory_renderer
```

| Endpoint | Request | Response |
|---|---|---|
| `products::list` | `GET /api/products` | `200`, the products |
| `products::update` | `PUT /api/products/{id}`, `{"name": "...", "price": 1.5, "stock": 3}` | `200`, the product; `422` problem for an empty name or a negative figure, `404` for no product |
| `products::sales` | `GET /api/products/{id}/sales` | `200`, `{"months": [...], "units": [...]}`; `404` for no product |

## The page

| File | What it holds |
|---|---|
| `web/main.cpp` | `mount<dashboard_page>()`, in `#app` |
| `web/components/dashboard_page.hpp` | the products, the one chosen, its sales or why they did not load, how each product's saves go, and the calls that change them |
| `web/components/product_list.hpp` | the filter, a state of its own, and the rows of the products it matches |
| `web/components/product_row.hpp` | one product, with its `on_select` callback |
| `web/components/product_detail.hpp` | the fields of the product chosen, what is typed kept until saved, and kept when typed while a save is on its way; its save waiting or why it failed, from the page; the `on_save` callback |
| `web/components/field.hpp` | a labelled input showing a value it is given, with an `on_change` callback |
| `web/components/sales_chart.hpp`, `.cpp` | the `line_chart` widget, and the month of the point clicked; its view in a source of its own, as the widget's JSON goes through Glaze |
| `web/widgets/line_chart.js` | the widget's module: `mount`, `update` and `unmount` over chart.js, a click emitting `point_selected` |
| `web/products_client.hpp`, `.cpp` | the calls to the server, through a `rest::client` of the contract, in a source of their own |

The detail and the chart are keyed by the product: another product chosen is another detail, with nothing typed, and
another chart. Only the sales asked for last show, or their failure: a product chosen again, or another, while some are
on their way; their failure shows where they would. A product saves once at a time: the page keeps which saves are on
their way and why a product's last one failed until the next, and gives each detail its own, so that one chosen again
shows its save waiting, or that it was not saved. A field changed while the save is on its way keeps what it holds once
it is answered.

## Run it

As [the notes page](../notes/web/README.md#run-it), with `samples/dashboard/web/dev.sh`: it builds and starts the
products server on port 8080, then Vite on port 5173. A change of a literal shows in some milliseconds; anything else
takes a rebuild of about 2.9 s on a 4-core i5, from a save to the page shown again. The chart's view is in a source of
its own, so that the widget's JSON does not compile again with the rest of the page.

```bash
cmake -B build && cmake --build build -j
./build/samples/dashboard/quoll_dashboard_server 8080 &
curl localhost:8080/api/products/1/sales
```

`ctest --test-dir build -R quoll` runs its tests with the others: `tests/service_test.cpp` calls the service through a
REST client over a router of the same program, and `tests/dashboard_page_test.cpp` runs the page natively, the calls
to the server defined in the test over products in memory, the chart logged by `memory_renderer`.

## What the screen lacked

Written with what Quoll has, step 6 of [the plan](../../docs/PLAN.md#step-6-the-ui-dsl-complete). It needed nothing that
JavaScript widgets do not give: chart.js takes its data as props, and a click comes back as an event, with no handle on
the widget and no WebSocket. What took code of its own, for base components to give:

- **An input showing a value it is given.** `bind()` holds a `state<std::string>`; a prop goes in through a decorator
  of the page's own (`target.value = ...` in `field.hpp`), and what is typed through `on_input`.
- **A labelled field, a number field.** `field.hpp` is the first base component the page wanted. The price and the stock
  are parsed and formatted by hand (`std::from_chars`, `std::format`), and refused with a message of the detail's own.
- **States that start from the props.** `mounted()` has no props, so the detail keeps what is typed as
  `state<std::optional<std::string>>`, nothing until a field changes, and is keyed by the product so that another
  starts empty.
- **Requests that overlap.** The sales of a product chosen before the last may answer, or fail, after it; two saves of
  a product may be answered out of order, or sent by a double click; one request's success may clear another's failure.
  The page counts the sales it asks for and shows only the last one's answer, keeps how each product's saves go, one at
  a time, and the detail compares what each field holds with what it sent before forgetting it. What would take the
  place of this bookkeeping, for `quoll::ui` to give:
  - a task that a new one of its kind stops, for the last request only: stopped, it never resumes, so that a stale
    answer writes nothing;
  - an action that runs once at a time, and says whether it is under way, for a button to wait on;
  - a state that is loading, a value or a failure, loaded again as what it depends on changes;
  - a draft of a form, from the props, that tells what changed since it was sent.
- **What a child tells, one task later.** A callback runs as a task of the component that wrote it: a field's change
  reaches the detail after the event that made it, so that a save dispatched in the same turn, as a test does, reads the
  value before it.
