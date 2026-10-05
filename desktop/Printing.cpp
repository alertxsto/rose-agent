#include "Printing.h"
#include <QFileInfo>
#include <QGraphicsScene>
#include <QImage>
#include <QPainter>
#include <QPrintDialog>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QSvgGenerator>
#include <cmath>
namespace rose::desktop {
namespace {
void render(QGraphicsScene *scene, QPainter &painter, QRectF target) {
    painter.setRenderHint(QPainter::Antialiasing);
    scene->render(&painter,target,scene->itemsBoundingRect().adjusted(-16,-16,16,16),Qt::KeepAspectRatio);
}
}
void printDiagram(QGraphicsScene *scene,QWidget *parent,bool preview) {
    QPrinter printer(QPrinter::HighResolution);
    auto paint=[scene](QPrinter *device){QPainter painter(device);if(painter.isActive())render(scene,painter,device->pageLayout().paintRectPixels(device->resolution()));};
    if(preview){QPrintPreviewDialog dialog(&printer,parent);QObject::connect(&dialog,&QPrintPreviewDialog::paintRequested,&dialog,paint);dialog.exec();}
    else {QPrintDialog dialog(&printer,parent);if(dialog.exec()==QDialog::Accepted)paint(&printer);}
}
bool exportDiagram(QGraphicsScene *scene,const QString &path,QString *error) {
    auto bounds=scene->itemsBoundingRect().adjusted(-16,-16,16,16);auto suffix=QFileInfo(path).suffix().toLower();
    if(suffix=="pdf") {QPrinter printer(QPrinter::HighResolution);printer.setOutputFormat(QPrinter::PdfFormat);printer.setOutputFileName(path);QPainter painter(&printer);if(!painter.isActive()){*error=QObject::tr("Cannot open PDF output");return false;}render(scene,painter,printer.pageLayout().paintRectPixels(printer.resolution()));if(!painter.end()){*error=QObject::tr("PDF output failed");return false;}return true;}
    const double scale=std::min(1.0,8192.0/std::max({1.0,bounds.width(),bounds.height()}));QSize size(std::max(1,int(std::ceil(bounds.width()*scale))),std::max(1,int(std::ceil(bounds.height()*scale))));
    if(suffix=="svg") {QSvgGenerator generator;generator.setFileName(path);generator.setSize(size);generator.setViewBox(QRect(QPoint(),size));QPainter painter(&generator);if(!painter.isActive()){*error=QObject::tr("Cannot open SVG output");return false;}render(scene,painter,QRectF(QPointF(),size));if(!painter.end()){*error=QObject::tr("SVG output failed");return false;}return true;}
    if(suffix=="png") {QImage image(size,QImage::Format_ARGB32_Premultiplied);if(image.isNull()){*error=QObject::tr("Cannot allocate export image");return false;}image.fill(Qt::white);QPainter painter(&image);render(scene,painter,QRectF(QPointF(),size));painter.end();if(!image.save(path,"PNG")){*error=QObject::tr("PNG output failed");return false;}return true;}
    *error=QObject::tr("Choose a PDF, SVG or PNG output filename");return false;
}
}
